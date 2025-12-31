import argparse
import json
import os
import re
import sys
import time
from dataclasses import dataclass
from difflib import SequenceMatcher
from typing import List, Optional, Dict, Any, Tuple

from llama_cpp import Llama


@dataclass
class Character:
    name: str
    desc: str = ""
    directions: str = ""


@dataclass
class Message:
    speaker: str
    text: str
    from_user: bool = False


DEFAULT_CHARACTERS = [
    Character(
        name="Milo",
        desc="A cat burglar who is trying to be sly.",
        directions="Be cautious, clever, and a little playful. Keep it short and natural.",
    ),
    Character(
        name="Otis",
        desc="A friendly security guard.",
        directions="Be calm, polite, observant, and helpful. Ask clarifying questions naturally.",
    ),
]

DEFAULT_HISTORY = [
    Message(speaker="Player", text="Hello Buddy", from_user=True),
    Message(speaker="Milo", text="Hey, what brings you here tonight?"),
    Message(speaker="Otis", text="Evening. Anything I can help you with?"),
]

MODEL_PATH = r"C:\Users\Phil Lopez\Documents\Unreal Projects\AutoChat\Plugins\LocalTalker\Resources\Models\Llama-3.2-3B-Q4_K_M.gguf"

N_CTX = 2048
N_THREADS = 4
N_GPU_LAYERS = 0

DEFAULT_MAX_TOKENS = 160
DEFAULT_TEMPERATURE = 0.65
DEFAULT_TOP_P = 0.90
DEFAULT_TOP_K = 40
DEFAULT_REPEAT_PENALTY = 1.12
DEFAULT_REPEAT_LAST_N = 128
DEFAULT_FREQUENCY_PENALTY = 0.08
DEFAULT_PRESENCE_PENALTY = 0.08


def now_ms() -> int:
    return int(time.time() * 1000)


def safe_print(s: str) -> None:
    if s is None:
        sys.stdout.write("\n")
        return
    b = s.encode("utf-8", errors="replace")
    sys.stdout.buffer.write(b)
    if not s.endswith("\n"):
        sys.stdout.buffer.write(b"\n")
    sys.stdout.buffer.flush()


def one_line(text: str) -> str:
    s = (text or "").replace("\r", " ").replace("\n", " ").replace("\t", " ")
    while "  " in s:
        s = s.replace("  ", " ")
    return s.strip()


def norm(text: str) -> str:
    return re.sub(r"\s+", " ", re.sub(r"[^a-z0-9'\" ]+", " ", (text or "").lower())).strip()


def similarity(a: str, b: str) -> float:
    a2 = norm(a)
    b2 = norm(b)
    if not a2 or not b2:
        return 0.0
    return SequenceMatcher(None, a2, b2).ratio()


def brief_for(c: Optional[Character]) -> str:
    if not c:
        return ""
    brief = c.desc if c.desc else c.directions
    brief = one_line(brief)
    if len(brief) > 220:
        brief = brief[:220] + "..."
    return brief


def sanitize_history(history: List[Message]) -> List[Message]:
    out: List[Message] = []
    for m in history:
        t = one_line(m.text)
        if not t:
            continue
        if t in ("…", "...", "[NO_OUTPUT]"):
            continue
        if t.endswith(": …") or t.endswith(": ..."):
            continue
        out.append(Message(speaker=m.speaker, text=t, from_user=m.from_user))
    return out


def trim_history(history: List[Message], max_history_messages: int, max_context_chars: int) -> List[Message]:
    h = list(history)
    if max_history_messages > 0 and len(h) > max_history_messages:
        h = h[-max_history_messages:]
    if max_context_chars > 0:
        budget = sum(len(m.speaker) + len(m.text) + 8 for m in h)
        while h and budget > max_context_chars:
            m0 = h.pop(0)
            budget -= len(m0.speaker) + len(m0.text) + 8
    return h


def build_transcript(history: List[Message], max_lines: int = 20) -> str:
    lines = []
    for m in history[-max_lines:]:
        speaker = "Player" if m.from_user else (m.speaker or "Unknown")
        t = one_line(m.text)
        if not t:
            continue
        lines.append(f"{speaker}: {t}")
    return "\n".join(lines)


def pick_last_non_self(history: List[Message], self_name: str) -> Optional[Message]:
    sn = (self_name or "").strip().lower()
    for m in reversed(history):
        if m.from_user:
            return m
        sp = (m.speaker or "").strip().lower()
        if sp != sn:
            return m
    return None


def last_spoken_lines(history: List[Message], k: int = 2) -> List[str]:
    out = []
    for m in reversed(history):
        if m.from_user:
            continue
        t = one_line(m.text)
        if t:
            out.append(t)
        if len(out) >= k:
            break
    return list(reversed(out))


def build_messages_for_turn(
    speaker: str,
    characters: List[Character],
    history: List[Message],
    scene: str,
    directions: str,
    persona: str,
    max_history_messages: int,
    max_context_chars: int,
) -> List[Dict[str, str]]:
    speaker = (speaker or "").strip()
    chars = [c for c in characters if c and c.name and c.name.strip()]

    hist0 = sanitize_history(history)
    hist = trim_history(hist0, max_history_messages, max_context_chars)

    self_char = next((c for c in chars if c.name.lower() == speaker.lower()), None)
    others = [c for c in chars if c.name.lower() != speaker.lower()]

    cast_bits = []
    if self_char:
        cast_bits.append(f"{speaker}: {brief_for(self_char)}")
    for c in others:
        cast_bits.append(f"{c.name}: {brief_for(c)}")
    cast = " | ".join(cast_bits) if cast_bits else ""

    recent = last_spoken_lines(hist, k=2)
    recent_block = "\n".join([f"- {one_line(x)}" for x in recent]) if recent else "- (none)"

    last_other = pick_last_non_self(hist, speaker)
    last_other_line = ""
    if last_other:
        spk = "Player" if last_other.from_user else (last_other.speaker or "Unknown")
        last_other_line = f"{spk}: {one_line(last_other.text)}"

    system = (
        f"You are {speaker}. Stay in character.\n"
        "You are writing spoken dialogue in a realistic conversation.\n"
        "Do not write narration, stage directions, thoughts, headings, or lists.\n"
        "Do not mention prompts, rules, or being an AI.\n"
        "Keep it natural and specific. 1-3 sentences.\n"
        "Do not repeat or paraphrase the last two spoken lines.\n"
        "Do not speak for other characters.\n"
        "Include one concrete detail from the scene (sound, object, movement, or place).\n"
    )
    if persona.strip():
        system += persona.strip() + "\n"
    if directions.strip():
        system += directions.strip() + "\n"

    user = ""
    if scene.strip():
        user += f"Scene: {one_line(scene)}\n"
    if cast:
        user += f"Cast briefs: {cast}\n"
    user += "Transcript so far:\n"
    user += build_transcript(hist, max_lines=20) + "\n\n"
    user += f"Recent lines (do not repeat/paraphrase):\n{recent_block}\n\n"
    if last_other_line:
        user += f"Reply to the last message (do not quote it): {last_other_line}\n"
    user += "Respond now."

    return [
        {"role": "system", "content": system.strip()},
        {"role": "user", "content": user.strip()},
    ]


def minimal_clean_output(text: str, speaker: str) -> str:
    t = (text or "").strip()
    if not t:
        return ""
    # very light cleanup only
    t = t.replace("\r", "\n").strip()
    # if the model included "Speaker:" prefix, strip it
    if t.lower().startswith(speaker.lower() + ":"):
        t = t[len(speaker) + 1 :].lstrip()
    return t.strip()


def is_bad_output(text: str) -> bool:
    t = (text or "").strip().lower()
    if not t:
        return True
    if "you are " in t[:40] or t.startswith(("rules:", "scene:", "transcript")):
        return True
    if "as an ai" in t or "language model" in t:
        return True
    return False


def call_chat(
    llm: Llama,
    messages: List[Dict[str, str]],
    max_tokens: int,
    temperature: float,
    top_p: float,
    top_k: int,
    repeat_penalty: float,
    repeat_last_n: int,
    frequency_penalty: float,
    presence_penalty: float,
    stop: List[str],
    seed: Optional[int],
    log: bool,
) -> Dict[str, Any]:
    t0 = now_ms()
    resp = llm.create_chat_completion(
        messages=messages,
        max_tokens=max_tokens,
        temperature=temperature,
        top_p=top_p,
        top_k=top_k,
        repeat_penalty=repeat_penalty,
        repeat_last_n=repeat_last_n,
        frequency_penalty=frequency_penalty,
        presence_penalty=presence_penalty,
        stop=stop,
        seed=seed,
    )
    t1 = now_ms()
    content = resp["choices"][0]["message"]["content"]

    out = {
        "duration_ms": t1 - t0,
        "content": content,
        "usage": resp.get("usage"),
        "stop": stop,
    }

    if log:
        print("\n[LLM_CALL]")
        print(f"  duration_ms={out['duration_ms']}")
        if out["usage"] is not None:
            print("  usage:", out["usage"])
        print("  stop:", stop)
        print("[RAW_OUTPUT_BEGIN]")
        safe_print(content)
        print("[RAW_OUTPUT_END]")

    return out


def generate_turn(
    llm: Llama,
    characters: List[Character],
    history: List[Message],
    scene: str,
    speaker: str,
    args: argparse.Namespace,
) -> str:
    seed = None if args.seed < 0 else args.seed

    messages = build_messages_for_turn(
        speaker=speaker,
        characters=characters,
        history=history,
        scene=scene,
        directions=args.directions,
        persona=args.persona,
        max_history_messages=args.max_history,
        max_context_chars=args.max_chars,
    )

    other_names = [c.name for c in characters if c.name and c.name.lower() != speaker.lower()]

    # Stop if model tries to start speaking as someone else or starts a new header-ish blob.
    stop = [
        "<|eot_id|>",
        "\n###",
        "\n##",
        "\n# ",
        "\nRe:",
        "\nFW:",
        "\nFWD:",
        "\nby ",
        "http://",
        "https://",
        "\nPlayer:",
    ]
    for n in other_names:
        stop.append(f"\n{n}:")
        stop.append(f"{n}:")
    stop.append("<|start_header_id|>")

    if args.log:
        print("\n" + "=" * 90)
        print(f"[TURN] i={args._turn_index} speaker={speaker}")
        print("[SYSTEM_BEGIN]")
        print(messages[0]["content"])
        print("[SYSTEM_END]")
        print("[USER_BEGIN]")
        print(messages[1]["content"])
        print("[USER_END]")

    r1 = call_chat(
        llm=llm,
        messages=messages,
        max_tokens=args.max_tokens,
        temperature=args.temperature,
        top_p=args.top_p,
        top_k=args.top_k,
        repeat_penalty=args.repeat_penalty,
        repeat_last_n=args.repeat_last_n,
        frequency_penalty=args.frequency_penalty,
        presence_penalty=args.presence_penalty,
        stop=stop,
        seed=seed,
        log=args.log,
    )
    out = minimal_clean_output(r1["content"], speaker)

    if is_bad_output(out):
        # light retry: shorter, more direct user message, same system
        retry_user = (
            f"Scene: {one_line(scene)}\n"
            f"Last line: {one_line(pick_last_non_self(sanitize_history(history), speaker).text) if pick_last_non_self(sanitize_history(history), speaker) else ''}\n"
            "Respond naturally in character now. 1-3 sentences. No narration."
        ).strip()
        messages2 = [messages[0], {"role": "user", "content": retry_user}]

        if args.log:
            print("\n[RETRY]")
            print("[USER2_BEGIN]")
            print(retry_user)
            print("[USER2_END]")

        r2 = call_chat(
            llm=llm,
            messages=messages2,
            max_tokens=args.max_tokens,
            temperature=max(0.50, args.temperature - 0.10),
            top_p=args.top_p,
            top_k=args.top_k,
            repeat_penalty=max(args.repeat_penalty, 1.10),
            repeat_last_n=args.repeat_last_n,
            frequency_penalty=args.frequency_penalty,
            presence_penalty=args.presence_penalty,
            stop=stop,
            seed=seed,
            log=args.log,
        )
        out2 = minimal_clean_output(r2["content"], speaker)
        if not is_bad_output(out2):
            out = out2

    return out.strip()


def main() -> int:
    ap = argparse.ArgumentParser(description="Multi-turn dialogue using llama-cpp-python chat completion and Llama 3 templates.")
    ap.add_argument("--turns", type=int, default=10)
    ap.add_argument("--start", default="Milo")
    ap.add_argument("--scene", default="A quiet museum corridor at night; faint camera hum; a locked glass display case nearby.")
    ap.add_argument("--max-tokens", type=int, default=DEFAULT_MAX_TOKENS)
    ap.add_argument("--temperature", type=float, default=DEFAULT_TEMPERATURE)
    ap.add_argument("--top-p", type=float, default=DEFAULT_TOP_P)
    ap.add_argument("--top-k", type=int, default=DEFAULT_TOP_K)
    ap.add_argument("--repeat-penalty", type=float, default=DEFAULT_REPEAT_PENALTY)
    ap.add_argument("--repeat-last-n", type=int, default=DEFAULT_REPEAT_LAST_N)
    ap.add_argument("--frequency-penalty", type=float, default=DEFAULT_FREQUENCY_PENALTY)
    ap.add_argument("--presence-penalty", type=float, default=DEFAULT_PRESENCE_PENALTY)
    ap.add_argument("--max-history", type=int, default=20)
    ap.add_argument("--max-chars", type=int, default=2200)
    ap.add_argument("--directions", default="")
    ap.add_argument("--persona", default="")
    ap.add_argument("--history-json", default="")
    ap.add_argument("--seed", type=int, default=-1)
    ap.add_argument("--log", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(MODEL_PATH):
        raise SystemExit(f"Model not found: {MODEL_PATH}")

    characters = list(DEFAULT_CHARACTERS)
    history = list(DEFAULT_HISTORY)

    if args.history_json:
        with open(args.history_json, "r", encoding="utf-8") as f:
            raw = json.load(f)
        history = [Message(**m) for m in raw]

    llm = Llama(
        model_path=MODEL_PATH,
        n_ctx=N_CTX,
        n_threads=N_THREADS,
        n_gpu_layers=N_GPU_LAYERS,
        chat_format="llama-3",  # important: apply Llama 3 template
        verbose=False,
    )

    order = ["Milo", "Otis"]
    if args.start.strip().lower() == "otis":
        order = ["Otis", "Milo"]

    scene = one_line(args.scene)

    print("\n=== MULTI-TURN TEST ===\n")
    print("Scene:")
    print(scene)
    print("\nInitial context:")
    for m in history:
        who = "Player" if m.from_user else m.speaker
        print(f"{who}: {one_line(m.text)}")
    print("")

    for i in range(args.turns):
        args._turn_index = i
        speaker = order[i % 2]

        line = generate_turn(
            llm=llm,
            characters=characters,
            history=history,
            scene=scene,
            speaker=speaker,
            args=args,
        )

        if line:
            safe_print(f"{speaker}: {line}")
            history.append(Message(speaker=speaker, text=line, from_user=False))
        else:
            print(f"{speaker}: [NO_OUTPUT] (skipped appending)")

    print("\n=== END ===\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
