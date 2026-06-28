#!/usr/bin/env python3
"""Search backend for the OCLI C++ build. Runs a robust web search via the maintained
ddgs library (which handles DuckDuckGo's anti-scraping) and prints JSON to stdout so the
C++ web_search tool can consume it.

Usage:   search_backend.py "<query>" [max_results]
Output:  {"results": [{"title","href","body"}, ...]}   or   {"error": "...", "results": [...]}
"""
import sys
import json


def get_ddgs():
    try:
        from ddgs import DDGS
        return DDGS
    except Exception:
        pass
    try:
        from duckduckgo_search import DDGS
        return DDGS
    except Exception:
        return None


def run(query, num):
    DDGS = get_ddgs()
    if DDGS is None:
        return {"error": "no search library (install: pip install ddgs)", "results": []}
    out = []
    # ddgs has shifted kwargs across versions; try the rich call, then degrade.
    attempts = (
        dict(region="wt-wt", safesearch="moderate", max_results=num),
        dict(max_results=num),
        dict(),
    )
    last_err = ""
    for kw in attempts:
        try:
            with DDGS() as ddgs:
                for r in ddgs.text(query, **kw):
                    out.append({
                        "title": r.get("title", "") or "",
                        "href": r.get("href", "") or r.get("url", "") or "",
                        "body": r.get("body", "") or r.get("snippet", "") or "",
                    })
                    if len(out) >= num:
                        break
            if out:
                return {"results": out}
        except TypeError as e:
            last_err = str(e)
            continue
        except Exception as e:
            last_err = str(e)
            continue
    return {"error": last_err, "results": out}


def main():
    if len(sys.argv) < 2 or not sys.argv[1].strip():
        print(json.dumps({"error": "no query", "results": []}))
        return
    query = sys.argv[1]
    try:
        num = int(sys.argv[2])
    except Exception:
        num = 20
    if num < 1:
        num = 1
    if num > 50:
        num = 50
    try:
        print(json.dumps(run(query, num)))
    except Exception as e:
        print(json.dumps({"error": str(e), "results": []}))


if __name__ == "__main__":
    main()
