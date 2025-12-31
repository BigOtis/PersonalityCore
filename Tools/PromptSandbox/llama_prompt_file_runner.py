import argparse
import os
import sys
from llama_cpp import Llama, LlamaGrammar

MODEL_PATH = r"C:\Users\Phil Lopez\Documents\Unreal Projects\AutoChat\Plugins\LocalTalker\Resources\Models\Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf"
N_CTX = 4096
N_THREADS = 4
N_GPU_LAYERS = 0

DEFAULT_MAX_TOKENS       = 64
DEFAULT_TEMPERATURE      = 0.4
DEFAULT_TOP_P            = 0.88
DEFAULT_TOP_K            = 40
DEFAULT_REPEAT_PENALTY   = 1.15
DEFAULT_FREQUENCY_PENALTY= 0.14
DEFAULT_PRESENCE_PENALTY = 0.06


def safe_print(text: str) -> None:
    if text is None:
        sys.stdout.write("\n")
        return
    data = text.encode("utf-8", errors="replace")
    sys.stdout.buffer.write(data)
    if not text.endswith("\n"):
        sys.stdout.buffer.write(b"\n")
    sys.stdout.buffer.flush()


def main() -> int:
    ap = argparse.ArgumentParser(description="Raw prompt file runner (you provide full Llama3 prompt tokens).")
    ap.add_argument("--prompt", required=True, help="Path to prompt text file")
    ap.add_argument("--max-tokens", type=int, default=DEFAULT_MAX_TOKENS)
    ap.add_argument("--temperature", type=float, default=DEFAULT_TEMPERATURE)
    ap.add_argument("--top-p", type=float, default=DEFAULT_TOP_P)
    ap.add_argument("--top-k", type=int, default=DEFAULT_TOP_K)
    ap.add_argument("--repeat-penalty", type=float, default=DEFAULT_REPEAT_PENALTY)
    ap.add_argument("--frequency-penalty", type=float, default=DEFAULT_FREQUENCY_PENALTY)
    ap.add_argument("--presence-penalty", type=float, default=DEFAULT_PRESENCE_PENALTY)
    ap.add_argument("--seed", type=int, default=-1)
    ap.add_argument("--stop", action="append", default=[], help="Add a stop sequence (repeatable)")
    args = ap.parse_args()

    if not os.path.exists(MODEL_PATH):
        raise SystemExit(f"Model not found: {MODEL_PATH}")

    prompt = open(args.prompt, "r", encoding="utf-8").read()

    llm = Llama(
        model_path=MODEL_PATH,
        n_ctx=N_CTX,
        n_threads=N_THREADS,
        n_gpu_layers=N_GPU_LAYERS,
        verbose=False,
    )

    otis_grammar = LlamaGrammar.from_string(
        r"""
root ::= "[OTIS]" space text space "[/OTIS]"
space ::= " "*
text ::= char+
char ::= [^\n\r\[\]]
"""
    )

    seed = None if args.seed < 0 else args.seed
    default_stop = [
        "<|eot_id|>",
        "\n[MILO]",
        "\n[PLAYER]",
        "\n[OTIS]",
        "\n[TRANSCRIPT]",
        "\n[/TRANSCRIPT]",
        "\nWhat would Otis say next?",
        "\n<|start_header_id|>",
        "\n<|begin_of_text|>",
    ]
    stop = default_stop + (args.stop if args.stop else [])

    resp = llm(
        prompt=prompt,
        max_tokens=args.max_tokens,
        temperature=args.temperature,
        top_p=args.top_p,
        top_k=args.top_k,
        repeat_penalty=args.repeat_penalty,
        frequency_penalty=args.frequency_penalty,
        presence_penalty=args.presence_penalty,
        stop=stop,
        grammar=otis_grammar,
        seed=seed,
    )
    text = resp["choices"][0]["text"]
    idx = text.find("[/OTIS]")
    if idx != -1:
        text = text[: idx + len("[/OTIS]")]
    safe_print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
