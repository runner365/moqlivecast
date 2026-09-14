/**
 * HTTP/3 客户端测试 — Node.js
 *
 * Node.js 目前不原生支持 HTTP/3，通过 curl 命令代理测试。
 * 真正的 HTTP/3 JS 客户端是浏览器 — 打开 h3_client.html。
 *
 * 用法:
 *   node h3_client.js [base_url] [curl_path]
 * ======================================== */

const BASE = process.argv[2] || "https://127.0.0.1:4433";
const CURL = process.argv[3] || "/opt/homebrew/opt/curl/bin/curl";
const { spawnSync } = require("node:child_process");

function curl(path, opts = {}) {
  const args = [CURL, "--http3-only", "-k", "-s", "-D-"];
  if (opts.method === "POST") {
    args.push("-X", "POST");
    args.push("-d", opts.body || "");
  }
  if (opts.ct) args.push("-H", "Content-Type: " + opts.ct);
  args.push(BASE + path);

  const r = spawnSync(CURL, args.slice(1), {
    encoding: "utf-8", timeout: 15000,
    maxBuffer: 1024 * 1024,
  });

  const out = r.stdout || "";
  const m = out.match(/^HTTP\/\d(?:\.\d)?\s+(\d+)/m);
  const status = m ? parseInt(m[1], 10) : 0;
  const bodyStart = out.indexOf("\r\n\r\n");
  const body = bodyStart >= 0 ? out.substring(bodyStart + 4) : out;
  return { status, body };
}

async function main() {
  console.log("╔══════════════════════════════════════════════╗");
  console.log("║   HTTP/3 JS Client — Node.js (via curl)      ║");
  console.log("╠══════════════════════════════════════════════╣");
  console.log(`║   Target: ${BASE}`);
  console.log(`║   Curl:   ${CURL}`);
  console.log("╚══════════════════════════════════════════════╝\n");

  let passed = 0, failed = 0;
  const check = (c, m) => { if (!c) throw new Error(m || "assertion failed"); };

  const tests = [
    {
      name: "GET /get/hello?name=alex&age=23",
      fn: () => {
        const { status, body } = curl("/get/hello?name=alex&age=23");
        check(status === 200, `status=${status}`);
        check(body.includes("alex"), "missing name=alex");
        check(body.includes("23"), "missing age=23");
        return { status, body };
      },
    },
    {
      name: "POST /post/hello (JSON)",
      fn: () => {
        const json = '{"name":"alex","age":23}';
        const { status, body } = curl("/post/hello", {
          method: "POST", ct: "application/json", body: json,
        });
        check(status === 200, `status=${status}`);
        return { status, body };
      },
    },
    {
      name: "POST /post/hello (plain text)",
      fn: () => {
        const txt = "hello from Node.js h3_client";
        const { status, body } = curl("/post/hello", {
          method: "POST", ct: "text/plain", body: txt,
        });
        check(status === 200, `status=${status}`);
        return { status, body };
      },
    },
    {
      name: "GET /nope → 404",
      fn: () => {
        const { status, body } = curl("/nope");
        check(status === 404, `expected 404, got ${status}`);
        check(body.includes("404"), "missing 404 text");
        return { status, body };
      },
    },
  ];

  for (const t of tests) {
    console.log(`── ${t.name} ──`);
    try {
      const { status, body } = t.fn();
      console.log(`    status: ${status}  PASS`);
      for (const l of body.trim().split("\n")) console.log(`    │ ${l}`);
      passed++;
    } catch (e) {
      console.log(`    FAIL: ${e.message}`);
      failed++;
    }
    console.log("");
  }

  console.log(`║   Results: ${passed} passed, ${failed} failed`);
  process.exit(failed > 0 ? 1 : 0);
}

main();
