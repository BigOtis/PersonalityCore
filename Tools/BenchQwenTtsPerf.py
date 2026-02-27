#!/usr/bin/env python3
"""
Qwen TTS performance benchmark for LocalTalker.

This stays Python-only and focuses on:
1) baseline synthesis timings (warm worker)
2) chunked synthesis first-audio latency
3) overlap scheduling simulation (generate-next while prior audio plays)
4) generation knob sweeps (e.g., max_new_tokens)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


def _resolve_hf_snapshot(repo_id: str) -> Optional[Path]:
    if not repo_id or "://" in repo_id:
        return None
    path_like = Path(repo_id)
    if path_like.exists():
        return None
    norm = repo_id.replace("\\", "/")
    if "/" not in norm or norm.startswith("./") or norm.startswith("../"):
        return None

    escaped = norm.replace("/", "--")
    roots: List[Path] = []
    env_hub = os.environ.get("HUGGINGFACE_HUB_CACHE", "").strip()
    if env_hub:
        roots.append(Path(env_hub))
    env_hf_home = os.environ.get("HF_HOME", "").strip()
    if env_hf_home:
        roots.append(Path(env_hf_home) / "hub")
    env_user = os.environ.get("USERPROFILE", "").strip()
    if env_user:
        roots.append(Path(env_user) / ".cache" / "huggingface" / "hub")
    env_local = os.environ.get("LOCALAPPDATA", "").strip()
    if env_local:
        roots.append(Path(env_local) / "huggingface" / "hub")

    best: Optional[Tuple[float, Path]] = None
    for root in roots:
        snaps = root / f"models--{escaped}" / "snapshots"
        if not snaps.is_dir():
            continue
        for child in snaps.iterdir():
            if not child.is_dir():
                continue
            mtime = child.stat().st_mtime
            if best is None or mtime > best[0]:
                best = (mtime, child)
    return best[1] if best else None


def _line_chunks(text: str, max_chars: int) -> List[str]:
    chunks = [c.strip() for c in re.split(r"(?<=[.!?;:])\s+", text.strip()) if c.strip()]
    out: List[str] = []
    for c in chunks:
        if len(c) <= max_chars:
            out.append(c)
            continue
        words = c.split()
        cur: List[str] = []
        cur_len = 0
        for w in words:
            add = len(w) + (1 if cur else 0)
            if cur and cur_len + add > max_chars:
                out.append(" ".join(cur))
                cur = [w]
                cur_len = len(w)
            else:
                cur.append(w)
                cur_len += add
        if cur:
            out.append(" ".join(cur))
    return out if out else [text.strip()]


@dataclass
class SynthResult:
    elapsed_s: float
    audio_s: float
    ok: bool
    error: str
    model_s: float
    write_s: float
    total_s_worker: float
    cache_hit: bool

    @property
    def rtf(self) -> float:
        return (self.elapsed_s / self.audio_s) if self.audio_s > 0 else 0.0


class WorkerClient:
    def __init__(self, cmd: List[str]) -> None:
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def close(self) -> None:
        try:
            self.request({"cmd": "shutdown"}, timeout_s=10)
        except Exception:
            pass
        try:
            self.proc.terminate()
            self.proc.wait(timeout=2)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass

    def _read_json(self, timeout_s: float) -> Dict[str, Any]:
        t0 = time.perf_counter()
        while True:
            if (time.perf_counter() - t0) > timeout_s:
                raise TimeoutError("Timed out waiting for worker response.")
            line = self.proc.stdout.readline()
            if line == "":
                if self.proc.poll() is not None:
                    raise RuntimeError("Worker exited unexpectedly.")
                time.sleep(0.01)
                continue
            s = line.strip()
            if not s:
                continue
            try:
                return json.loads(s)
            except Exception:
                # Ignore noisy non-JSON stdout lines.
                continue

    def request(self, obj: Dict[str, Any], timeout_s: float = 240.0) -> Tuple[float, Dict[str, Any]]:
        if self.proc.stdin is None:
            raise RuntimeError("Worker stdin is unavailable.")
        t0 = time.perf_counter()
        self.proc.stdin.write(json.dumps(obj, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        resp = self._read_json(timeout_s=timeout_s)
        return time.perf_counter() - t0, resp


def _synthesize(
    wc: WorkerClient,
    outdir: Path,
    text: str,
    speaker: str,
    language: str,
    gen: Dict[str, Any],
    timeout_s: float,
) -> SynthResult:
    outwav = outdir / f"bench_{int(time.time() * 1000)}_{abs(hash(text)) % 1000000}.wav"
    req: Dict[str, Any] = {
        "cmd": "synthesize",
        "text": text,
        "language": language,
        "speaker": speaker,
        "output_wav": str(outwav),
        "non_streaming_mode": True,
    }
    req.update(gen)
    elapsed, resp = wc.request(req, timeout_s=timeout_s)

    if not resp.get("ok", False):
        return SynthResult(
            elapsed_s=elapsed,
            audio_s=0.0,
            ok=False,
            error=str(resp.get("error", "unknown_error")),
            model_s=0.0,
            write_s=0.0,
            total_s_worker=0.0,
            cache_hit=False,
        )

    sr = int(resp.get("sample_rate", 0) or 0)
    ns = int(resp.get("num_samples", 0) or 0)
    audio_s = (ns / sr) if sr > 0 else 0.0
    metrics = resp.get("metrics", {}) if isinstance(resp.get("metrics"), dict) else {}
    model_s = float(metrics.get("model_seconds", 0.0) or 0.0)
    write_s = float(metrics.get("write_seconds", 0.0) or 0.0)
    total_s_worker = float(metrics.get("total_seconds", 0.0) or 0.0)
    cache_hit = bool(metrics.get("cache_hit", False))

    try:
        outwav.unlink(missing_ok=True)
    except Exception:
        pass

    return SynthResult(
        elapsed_s=elapsed,
        audio_s=audio_s,
        ok=True,
        error="",
        model_s=model_s,
        write_s=write_s,
        total_s_worker=total_s_worker,
        cache_hit=cache_hit,
    )


def _synthesize_batch(
    wc: WorkerClient,
    outdir: Path,
    texts: List[str],
    speaker: str,
    language: str,
    gen: Dict[str, Any],
    timeout_s: float,
) -> Dict[str, Any]:
    items: List[Dict[str, Any]] = []
    for t in texts:
        outwav = outdir / f"bench_batch_{int(time.time() * 1000)}_{abs(hash(t)) % 1000000}.wav"
        items.append(
            {
                "text": t,
                "language": language,
                "speaker": speaker,
                "output_wav": str(outwav),
            }
        )

    req: Dict[str, Any] = {"cmd": "synthesize_batch", "items": items}
    req.update(gen)
    elapsed, resp = wc.request(req, timeout_s=timeout_s)
    if not resp.get("ok", False):
        return {"ok": False, "elapsed_s": elapsed, "error": str(resp.get("error", "unknown_error"))}

    metrics = resp.get("metrics", {}) if isinstance(resp.get("metrics"), dict) else {}
    out_items: List[Dict[str, Any]] = []
    for it in resp.get("items", []):
        sr = int(it.get("sample_rate", 0) or 0)
        ns = int(it.get("num_samples", 0) or 0)
        out_items.append(
            {
                "ok": bool(it.get("ok", False)),
                "audio_s": (ns / sr) if sr > 0 else 0.0,
                "sample_rate": sr,
                "num_samples": ns,
            }
        )

    for it in items:
        try:
            Path(it["output_wav"]).unlink(missing_ok=True)
        except Exception:
            pass

    return {
        "ok": True,
        "elapsed_s": elapsed,
        "items": out_items,
        "metrics": {
            "model_s": float(metrics.get("model_seconds", 0.0) or 0.0),
            "write_s": float(metrics.get("write_seconds", 0.0) or 0.0),
            "total_s": float(metrics.get("total_seconds", 0.0) or 0.0),
            "batch_size": int(metrics.get("batch_size", len(out_items)) or len(out_items)),
        },
    }


def _simulate_overlap(results: List[SynthResult]) -> Dict[str, float]:
    # Policy A: no overlap (synth starts only after previous audio fully finished)
    t = 0.0
    silence_no = 0.0
    for r in results:
        silence_no += r.elapsed_s
        play_start = t + r.elapsed_s
        t = play_start + r.audio_s
    makespan_no = t

    # Policy B: overlap generation (start synth_i when play_{i-1} starts)
    if not results:
        return {"silence_no_overlap_s": 0.0, "silence_overlap_s": 0.0, "makespan_no_overlap_s": 0.0, "makespan_overlap_s": 0.0}

    play_start_prev = results[0].elapsed_s
    play_end_prev = play_start_prev + results[0].audio_s
    silence_ov = results[0].elapsed_s

    for i in range(1, len(results)):
        gen_start = play_start_prev
        ready = gen_start + results[i].elapsed_s
        play_start = max(ready, play_end_prev)
        silence_ov += max(0.0, play_start - play_end_prev)
        play_start_prev = play_start
        play_end_prev = play_start + results[i].audio_s

    makespan_ov = play_end_prev
    return {
        "silence_no_overlap_s": silence_no,
        "silence_overlap_s": silence_ov,
        "makespan_no_overlap_s": makespan_no,
        "makespan_overlap_s": makespan_ov,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark Qwen TTS latency/throughput for LocalTalker.")
    parser.add_argument("--project-root", default=".", help="AutoChat project root.")
    parser.add_argument("--python", default="python", help="Python executable for worker.")
    parser.add_argument("--worker", default="Plugins/LocalTalker/Resources/Qwen/qwen_tts_worker.py")
    parser.add_argument("--model", default="Qwen/Qwen3-TTS-12Hz-0.6B-CustomVoice")
    parser.add_argument("--tokenizer", default="Qwen/Qwen3-TTS-Tokenizer-12Hz")
    parser.add_argument("--device", default="cuda:0")
    parser.add_argument("--dtype", default="bfloat16")
    parser.add_argument("--flash-attn", action="store_true", help="Enable flash_attention_2 load path.")
    parser.add_argument("--no-compile", action="store_true", help="Disable torch.compile in worker.")
    parser.add_argument("--cache-size", type=int, default=64, help="Worker in-memory synth cache size.")
    parser.add_argument("--speaker", default="vivian")
    parser.add_argument("--language", default="Auto")
    parser.add_argument("--max-chunk-chars", type=int, default=72)
    parser.add_argument("--max-new-tokens", default="4096,2048,1024", help="Comma list. Empty for default only.")
    parser.add_argument("--force-max-new-tokens", type=int, default=0, help="If >0, force this cap for all requests.")
    parser.add_argument("--do-sample", choices=["auto", "true", "false"], default="auto", help="Force do_sample for all requests.")
    parser.add_argument("--quick", action="store_true", help="Fast mode: fewer texts and one sweep.")
    parser.add_argument("--request-timeout", type=float, default=180.0, help="Per worker request timeout seconds.")
    parser.add_argument("--out-json", default="", help="Optional output json file.")
    args = parser.parse_args()

    root = Path(args.project_root).resolve()
    worker_path = (root / args.worker).resolve()
    outdir = root / "Saved" / "LocalTalkerBench"
    outdir.mkdir(parents=True, exist_ok=True)

    model = args.model
    tokenizer = args.tokenizer
    model_snap = _resolve_hf_snapshot(model)
    tok_snap = _resolve_hf_snapshot(tokenizer)
    if model_snap is not None:
        model = str(model_snap)
    if tok_snap is not None:
        tokenizer = str(tok_snap)
    elif model_snap is not None and "/" in args.tokenizer:
        tokenizer = str(model_snap)

    texts_all = [
        "HÃ¦ddu, Eva! Wha' brings ye to dis place?",
        "I'm here to ensure the security of this museum, Brutus. You're not on the guest list and your enthusiasm is not welcome in our halls.",
        "The temple entrance is guarded by two stone statues with blue eyes, and the control room is to your left behind the bronze gate.",
    ]
    texts = texts_all[:2] if args.quick else texts_all

    token_values: List[Optional[int]] = [None]
    for raw in [s.strip() for s in args.max_new_tokens.split(",") if s.strip()]:
        try:
            val = int(raw)
            if val > 0:
                token_values.append(val)
        except Exception:
            pass
    token_values = sorted(set(token_values), key=lambda x: 10**9 if x is None else x, reverse=True)
    if args.quick:
        token_values = [None]
    if int(args.force_max_new_tokens) > 0:
        token_values = [None]

    cmd = [
        args.python,
        str(worker_path),
        "--model", model,
        "--tokenizer", tokenizer,
        "--device", args.device,
        "--dtype", args.dtype,
        "--cache-size", str(max(0, int(args.cache_size))),
    ]
    cmd.append("--flash-attn" if args.flash_attn else "--no-flash-attn")
    cmd.append("--no-compile" if args.no_compile else "--compile")

    request_timeout = max(10.0, float(args.request_timeout))

    print("Worker cmd:", " ".join(f"\"{c}\"" if " " in c else c for c in cmd))
    wc = WorkerClient(cmd)
    report: Dict[str, Any] = {
        "model": model,
        "tokenizer": tokenizer,
        "device": args.device,
        "dtype": args.dtype,
        "cache_size": int(args.cache_size),
        "quick_mode": bool(args.quick),
        "request_timeout_s": request_timeout,
        "token_sweeps": [],
    }
    try:
        health_elapsed, health = wc.request({"cmd": "health"}, timeout_s=request_timeout)
        print(f"health: {health_elapsed:.2f}s ok={health.get('ok')}")
        report["health"] = {
            "elapsed_s": health_elapsed,
            "ok": bool(health.get("ok", False)),
        }
        if not health.get("ok", False):
            print("health error:", health)
            return 2

        # Warm-up
        warm = _synthesize(
            wc=wc,
            outdir=outdir,
            text="Ready.",
            speaker=args.speaker,
            language=args.language,
            gen={},
            timeout_s=request_timeout,
        )
        print(f"warmup: {warm.elapsed_s:.2f}s ok={warm.ok} rtf={warm.rtf:.2f}x")
        report["warmup"] = {
            "elapsed_s": warm.elapsed_s,
            "audio_s": warm.audio_s,
            "rtf": warm.rtf,
            "ok": warm.ok,
            "error": warm.error,
        }

        for max_new_tokens in token_values:
            gen: Dict[str, Any] = {}
            label = "default"
            if int(args.force_max_new_tokens) > 0:
                gen["max_new_tokens"] = int(args.force_max_new_tokens)
                label = f"forced_max_new_tokens={int(args.force_max_new_tokens)}"
            elif max_new_tokens is not None:
                gen["max_new_tokens"] = int(max_new_tokens)
                label = f"max_new_tokens={max_new_tokens}"
            if args.do_sample != "auto":
                gen["do_sample"] = (args.do_sample == "true")
                label += f", do_sample={gen['do_sample']}"

            full_results: List[SynthResult] = []
            print(f"\n=== Sweep: {label} ===")
            for i, txt in enumerate(texts, start=1):
                r = _synthesize(
                    wc=wc,
                    outdir=outdir,
                    text=txt,
                    speaker=args.speaker,
                    language=args.language,
                    gen=gen,
                    timeout_s=request_timeout,
                )
                full_results.append(r)
                status = "ok" if r.ok else "ERR"
                cache_flag = "hit" if r.cache_hit else "miss"
                print(f"full[{i}] {status} elapsed={r.elapsed_s:.2f}s audio={r.audio_s:.2f}s rtf={r.rtf:.2f}x model={r.model_s:.2f}s cache={cache_flag}")

            # Cache-hit benchmark on a short repeated line.
            cache_text = texts[0]
            cache_miss = _synthesize(
                wc=wc,
                outdir=outdir,
                text=cache_text,
                speaker=args.speaker,
                language=args.language,
                gen={**gen, "use_cache": False},
                timeout_s=request_timeout,
            )
            cache_hit = _synthesize(
                wc=wc,
                outdir=outdir,
                text=cache_text,
                speaker=args.speaker,
                language=args.language,
                gen={**gen, "use_cache": True},
                timeout_s=request_timeout,
            )
            print(
                f"cache: miss={cache_miss.elapsed_s:.2f}s hit={cache_hit.elapsed_s:.2f}s "
                f"speedup={(cache_miss.elapsed_s / max(cache_hit.elapsed_s, 1e-6)):.1f}x"
            )

            # Batch benchmark on first 2 lines.
            if args.quick:
                batch_two = {"ok": False, "skipped": True, "reason": "quick_mode"}
                print("batch2: skipped (quick mode)")
            else:
                seq_two_elapsed = full_results[0].elapsed_s + full_results[1].elapsed_s
                batch_two = _synthesize_batch(
                    wc=wc,
                    outdir=outdir,
                    texts=texts[:2],
                    speaker=args.speaker,
                    language=args.language,
                    gen=gen,
                    timeout_s=request_timeout,
                )
                if batch_two.get("ok"):
                    b_elapsed = float(batch_two["elapsed_s"])
                    print(
                        f"batch2: sequential={seq_two_elapsed:.2f}s batch={b_elapsed:.2f}s "
                        f"speedup={(seq_two_elapsed / max(b_elapsed, 1e-6)):.2f}x"
                    )
                else:
                    print(f"batch2: ERR {batch_two.get('error', 'unknown')}")

            # Chunking benchmark on longest text.
            chunks: List[str] = []
            chunk_results: List[SynthResult] = []
            first_chunk_ready = 0.0
            all_chunks_elapsed = 0.0
            all_chunks_audio = 0.0
            full_line_ready = 0.0
            if args.quick:
                print("chunking: skipped (quick mode)")
            else:
                long_text = max(texts, key=len)
                chunks = _line_chunks(long_text, max_chars=max(24, args.max_chunk_chars))
                for c in chunks:
                    chunk_results.append(
                        _synthesize(
                            wc=wc,
                            outdir=outdir,
                            text=c,
                            speaker=args.speaker,
                            language=args.language,
                            gen=gen,
                            timeout_s=request_timeout,
                        )
                    )
                base_long = full_results[texts.index(long_text)]
                first_chunk_ready = chunk_results[0].elapsed_s if chunk_results else 0.0
                all_chunks_elapsed = sum(r.elapsed_s for r in chunk_results)
                all_chunks_audio = sum(r.audio_s for r in chunk_results)
                full_line_ready = base_long.elapsed_s

            overlap = _simulate_overlap(full_results)
            silence_saved = overlap["silence_no_overlap_s"] - overlap["silence_overlap_s"]

            if not args.quick:
                print(f"chunking: chunks={len(chunks)} first_ready={first_chunk_ready:.2f}s vs full_ready={full_line_ready:.2f}s")
                print(f"chunking: all_chunks_elapsed={all_chunks_elapsed:.2f}s full_elapsed={full_line_ready:.2f}s")
            print(f"overlap: silence_no_overlap={overlap['silence_no_overlap_s']:.2f}s silence_overlap={overlap['silence_overlap_s']:.2f}s saved={silence_saved:.2f}s")

            report["token_sweeps"].append(
                {
                    "label": label,
                    "max_new_tokens": max_new_tokens,
                    "full": [
                        {
                            "elapsed_s": r.elapsed_s,
                            "audio_s": r.audio_s,
                            "rtf": r.rtf,
                            "ok": r.ok,
                            "error": r.error,
                            "model_s": r.model_s,
                            "write_s": r.write_s,
                            "worker_total_s": r.total_s_worker,
                            "cache_hit": r.cache_hit,
                        }
                        for r in full_results
                    ],
                    "cache": {
                        "miss_elapsed_s": cache_miss.elapsed_s,
                        "hit_elapsed_s": cache_hit.elapsed_s,
                        "speedup_x": (cache_miss.elapsed_s / max(cache_hit.elapsed_s, 1e-6)),
                        "miss_ok": cache_miss.ok,
                        "hit_ok": cache_hit.ok,
                    },
                    "batch2": batch_two,
                    "chunking": {
                        "skipped": bool(args.quick),
                        "chunks": chunks,
                        "chunk_results": [
                            {
                                "elapsed_s": r.elapsed_s,
                                "audio_s": r.audio_s,
                                "rtf": r.rtf,
                                "ok": r.ok,
                                "error": r.error,
                            }
                            for r in chunk_results
                        ],
                        "first_chunk_ready_s": first_chunk_ready,
                        "full_line_ready_s": full_line_ready,
                        "all_chunks_elapsed_s": all_chunks_elapsed,
                        "all_chunks_audio_s": all_chunks_audio,
                    },
                    "overlap": overlap,
                }
            )

    finally:
        wc.close()

    if args.out_json:
        out_json = Path(args.out_json)
        out_json.parent.mkdir(parents=True, exist_ok=True)
        out_json.write_text(json.dumps(report, indent=2), encoding="utf-8")
        print("\nWrote:", out_json)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
