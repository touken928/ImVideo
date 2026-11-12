"""Run the installed WASM consumer in an isolated, local Chromium session."""

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import threading

from playwright.sync_api import sync_playwright


class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, *_args):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--results", type=Path, default=Path("build/wasm-results"))
    parser.add_argument("--chrome", help="Use an installed Chrome instead of Playwright's Chromium.")
    args = parser.parse_args()
    args.results.mkdir(parents=True, exist_ok=True)
    for name in ("runtime.html", "wasm_tests.js", "wasm_tests.wasm", "wasm_tests.data"):
        if not (args.build / name).is_file():
            parser.error(f"Missing WASM consumer output: {args.build / name}")

    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler, directory=str(args.build.resolve())))
    serving = threading.Thread(target=server.serve_forever, daemon=True)
    serving.start()
    report = {"passed": False, "tests": [], "errors": []}
    messages = []
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(
                executable_path=args.chrome,
                args=["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"],
            )
            try:
                page = browser.new_page()
                page.on("console", lambda message: messages.append(message.text))
                page.on("pageerror", lambda error: report["errors"].append(str(error)))
                page.goto(f"http://127.0.0.1:{server.server_port}/runtime.html")
                page.wait_for_function("window.wasmTestsFinished || window.wasmTestFailure", timeout=90_000)
                report.update(page.evaluate("""() => ({
                    isolated: crossOriginIsolated, tests: window.wasmTestResults,
                    failure: window.wasmTestFailure || null,
                    passed: !!window.wasmTestsFinished && !window.wasmTestFailure &&
                        window.wasmTestResults.length === 5 && window.wasmTestResults.every(test => test.passed)
                })"""))
                report["browser"] = browser.version
                report["passed"] = report["passed"] and report["isolated"] and not report["errors"]
                if not report["passed"]:
                    page.screenshot(path=str(args.results / "failure.png"))
            finally:
                browser.close()
    except Exception as error:
        report["errors"].append(str(error))
        report["passed"] = False
    finally:
        server.shutdown()
        server.server_close()
        serving.join(timeout=5)
        (args.results / "browser.log").write_text("\n".join(messages), encoding="utf-8")
        (args.results / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
