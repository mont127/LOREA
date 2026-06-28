#!/usr/bin/env python3
import json
import os
import sys

DEFAULT_BASE_URL = "https://integrate.api.nvidia.com/v1"
DEFAULT_MODEL = "z-ai/glm-5.1"

KEY_FILES = [
    os.path.expanduser("~/.config/lorea/nvidia_api_key"),
    os.path.expanduser("~/.nvidia_api_key"),
]


def emit(text):
    sys.stdout.write(text)
    sys.stdout.flush()


def resolve_key(req):
    key = req.get("api_key")
    if key:
        return key
    for env in ("NVIDIA_API_KEY", "NVAPI_KEY"):
        v = os.getenv(env)
        if v:
            return v.strip()
    for path in KEY_FILES:
        try:
            if os.path.exists(path):
                with open(path, "r") as f:
                    v = f.read().strip()
                if v:
                    return v
        except OSError:
            pass
    return None


def load_request():
    if len(sys.argv) > 1 and os.path.exists(sys.argv[1]):
        with open(sys.argv[1], "r") as f:
            return json.load(f)
    data = sys.stdin.read()
    return json.loads(data) if data.strip() else {}


def main():
    try:
        from openai import OpenAI
    except ImportError:
        emit("[NVIDIA ERROR] The 'openai' package is not installed. Run: pip install openai")
        return

    try:
        req = load_request()
    except Exception as e:
        emit("[NVIDIA ERROR] Could not read request: " + str(e))
        return

    model = req.get("model") or DEFAULT_MODEL
    messages = req.get("messages") or [{"role": "user", "content": ""}]
    api_key = resolve_key(req)
    if not api_key:
        emit("[NVIDIA ERROR] No API key. Set NVIDIA_API_KEY in your environment, or put the key "
             "in ~/.config/lorea/nvidia_api_key")
        return
    base_url = req.get("base_url") or os.getenv("NVIDIA_BASE_URL") or DEFAULT_BASE_URL
    temperature = req.get("temperature", 1)
    top_p = req.get("top_p", 1)
    max_tokens = req.get("max_tokens", 16384)

    client = OpenAI(base_url=base_url, api_key=api_key)

    try:
        completion = client.chat.completions.create(
            model=model,
            messages=messages,
            temperature=temperature,
            top_p=top_p,
            max_tokens=max_tokens,
            stream=True,
        )
    except Exception as e:
        emit("[NVIDIA ERROR] " + str(e))
        return

    in_thought = False
    try:
        for chunk in completion:
            if not getattr(chunk, "choices", None):
                continue
            if len(chunk.choices) == 0 or getattr(chunk.choices[0], "delta", None) is None:
                continue
            delta = chunk.choices[0].delta
            reasoning = getattr(delta, "reasoning_content", None)
            if reasoning:
                if not in_thought:
                    emit("<thought>")
                    in_thought = True
                emit(reasoning)
            content = getattr(delta, "content", None)
            if content is not None:
                if in_thought:
                    emit("</thought>\n")
                    in_thought = False
                emit(content)
    except Exception as e:
        emit("\n[NVIDIA ERROR] stream interrupted: " + str(e))
    finally:
        if in_thought:
            emit("</thought>\n")


main()
