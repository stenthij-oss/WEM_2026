"""Downloads the AI overlay's model (SD-Turbo, half precision), the tiny decoder and the depth ControlNet into
Saved/AIOverlay/models.

The Hugging Face client stalls on a slow or flaky connection; this resumes where it stopped and retries until every
file is complete. Run it again at any time: finished files are skipped.

    StreamDiffusion\\.venv\\Scripts\\python.exe Tools\\AIOverlay\\download_model.py
"""

import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

MODELS = Path(__file__).resolve().parents[2] / "Saved" / "AIOverlay" / "models"

# (repository, file in the repository, where it goes under MODELS[, revision]). The half-precision weights are saved
# under the names diffusers looks for, so the folder loads like the full-precision original at half the download.
FILES = [
    ("stabilityai/sd-turbo", "model_index.json", "sd-turbo/model_index.json"),
    ("stabilityai/sd-turbo", "scheduler/scheduler_config.json", "sd-turbo/scheduler/scheduler_config.json"),
    ("stabilityai/sd-turbo", "text_encoder/config.json", "sd-turbo/text_encoder/config.json"),
    ("stabilityai/sd-turbo", "text_encoder/model.fp16.safetensors", "sd-turbo/text_encoder/model.safetensors"),
    ("stabilityai/sd-turbo", "tokenizer/merges.txt", "sd-turbo/tokenizer/merges.txt"),
    ("stabilityai/sd-turbo", "tokenizer/special_tokens_map.json", "sd-turbo/tokenizer/special_tokens_map.json"),
    ("stabilityai/sd-turbo", "tokenizer/tokenizer_config.json", "sd-turbo/tokenizer/tokenizer_config.json"),
    ("stabilityai/sd-turbo", "tokenizer/vocab.json", "sd-turbo/tokenizer/vocab.json"),
    ("stabilityai/sd-turbo", "unet/config.json", "sd-turbo/unet/config.json"),
    ("stabilityai/sd-turbo", "unet/diffusion_pytorch_model.fp16.safetensors", "sd-turbo/unet/diffusion_pytorch_model.safetensors"),
    ("stabilityai/sd-turbo", "vae/config.json", "sd-turbo/vae/config.json"),
    ("stabilityai/sd-turbo", "vae/diffusion_pytorch_model.fp16.safetensors", "sd-turbo/vae/diffusion_pytorch_model.safetensors"),
    ("madebyollin/taesd", "config.json", "taesd/config.json"),
    ("madebyollin/taesd", "diffusion_pytorch_model.safetensors", "taesd/diffusion_pytorch_model.safetensors"),
    # SD-Turbo is SD 2.1 underneath, so its ControlNets are SD 2.1 ones. The repository's own weights are a pickle; Hugging
    # Face's conversion bot left a safetensors copy (already half precision) in pull request 5.
    ("thibaud/controlnet-sd21-depth-diffusers", "config.json", "controlnet-depth/config.json"),
    ("thibaud/controlnet-sd21-depth-diffusers", "diffusion_pytorch_model.safetensors",
     "controlnet-depth/diffusion_pytorch_model.safetensors", "refs/pr/5"),
]


def remote_size(url: str) -> int:
    request = urllib.request.Request(url, method="HEAD")
    with urllib.request.urlopen(request, timeout=60) as response:
        return int(response.headers["Content-Length"])


def fetch(url: str, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    total = remote_size(url)
    if target.exists() and target.stat().st_size == total:
        print(f"have  {target.relative_to(MODELS)}", flush=True)
        return

    part = target.with_name(target.name + ".part")
    for attempt in range(1, 101):
        done = part.stat().st_size if part.exists() else 0
        if done >= total:
            break
        request = urllib.request.Request(url, headers={"Range": f"bytes={done}-"})
        try:
            with urllib.request.urlopen(request, timeout=60) as response, open(part, "ab") as out:
                last_report = 0.0
                while True:
                    chunk = response.read(1 << 20)
                    if not chunk:
                        break
                    out.write(chunk)
                    done += len(chunk)
                    if time.time() - last_report > 10:
                        print(f"      {target.name}: {done / total:6.1%} of {total / 2**20:,.0f} MB", flush=True)
                        last_report = time.time()
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as error:
            print(f"      {target.name}: {error}; resuming (try {attempt})", flush=True)
            time.sleep(min(30, 2 * attempt))

    if part.stat().st_size != total:
        raise RuntimeError(f"{target.name} stopped at {part.stat().st_size} of {total} bytes")
    part.replace(target)
    print(f"got   {target.relative_to(MODELS)}", flush=True)


def main() -> int:
    for repository, name, local, *revision in FILES:
        revision = (revision or ["main"])[0].replace("/", "%2F")
        fetch(f"https://huggingface.co/{repository}/resolve/{revision}/{name}", MODELS / local)
    print(f"Done: {MODELS}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
