const { test } = require("node:test");
const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const { runInNewContext } = require("node:vm");

const source = readFileSync(join(__dirname, "../site/release.js"), "utf8");
const html = readFileSync(join(__dirname, "../site/index.html"), "utf8");
const repository = "https://github.com/gioeleminardi/CroccoSpectrum";

function release(tag, prerelease = false) {
  const version = tag.replace(/^v/, "");
  const filenames = [".AppImage", ".tar.gz", ".AppImage.sha256", ".tar.gz.sha256"]
    .map((suffix) => `croccospectrum-${version}-x86_64${suffix}`);
  if (prerelease) filenames.push("BUILD-INFO.json", `croccospectrum-${version}-source.tar.gz`,
    `croccospectrum-${version}-source.tar.gz.sha256`, "dependency-sources.tar.gz",
    "dependency-sources.tar.gz.sha256");
  return {
    tag_name: tag, prerelease, draft: false, published_at: "2026-10-09T12:00:00Z",
    body: "Build date: 2026-10-08T23:00:00Z\n", html_url: `${repository}/releases/tag/${tag}`,
    assets: filenames.map((name) => ({ name, state: "uploaded", size: 1048576,
      browser_download_url: `${repository}/releases/download/${tag}/${name}` })),
  };
}

async function load(stable, developments, failure = null) {
  const nodes = new Map([...html.matchAll(/\bid="([^"]+)"/g)]
    .map((match) => [match[1], { textContent: "", hidden: true, href: `${repository}/releases` }]));
  const requests = [];
  const context = {
    document: { getElementById: (id) => { assert.ok(nodes.has(id), `Missing element ${id}`); return nodes.get(id); } },
    AbortSignal,
    fetch: async (url) => {
      requests.push(url);
      const latest = url.endsWith("/latest");
      if (failure === (latest ? "stable" : "development")) throw new Error("offline");
      const data = latest ? stable : developments;
      return { ok: data !== null, status: data === null ? 404 : 200, json: async () => data };
    },
  };
  runInNewContext(source, context);
  await new Promise(setImmediate);
  return { nodes, requests, context };
}

test("stable and development downloads stay separate with many development releases", async () => {
  const builds = Array.from({ length: 100 }, (_, index) => release(`v0.2.0-dev.${index + 100}.g233be51`, true));
  const { nodes, requests } = await load(release("v0.1.6"), builds);
  assert.match(nodes.get("appimage-download").href, /\/v0\.1\.6\//);
  assert.match(nodes.get("development-appimage-download").href, /\/v0\.2\.0-dev\.199\.g233be51\//);
  assert.equal(nodes.get("release-badge").textContent, "Stable release");
  assert.equal(nodes.get("development-release-badge").textContent, "Development");
  assert.ok(requests.some((url) => url.endsWith("/latest")));
});

test("out-of-order publication, drafts and incomplete newer builds keep the newest good build", async () => {
  const newer = release("v0.2.0-dev.185.g233be51", true);
  newer.published_at = "2026-10-08T12:00:00Z";
  const older = release("v0.2.0-dev.184.g233be51", true);
  const draft = { ...release("v0.2.0-dev.186.g233be51", true), draft: true };
  const incomplete = release("v0.2.0-dev.187.g233be51", true);
  incomplete.assets.pop();
  const { nodes } = await load(release("v0.1.6"), [older, draft, incomplete, newer]);
  assert.equal(nodes.get("development-release-notes").href, newer.html_url);
  assert.match(nodes.get("development-release-status").textContent, /Built Oct 8, 2026 · Commit 233be51/);
});

test("missing, empty and uploading checksums prevent incomplete downloads", async () => {
  for (const mutation of [
    (entry) => entry.assets.pop(),
    (entry) => { entry.assets[3].size = 0; },
    (entry) => { entry.assets[3].state = "new"; },
  ]) {
    const entry = release("v0.1.6");
    mutation(entry);
    const { nodes } = await load(entry, []);
    assert.equal(nodes.get("appimage-download").href, `${repository}/releases`);
    assert.match(nodes.get("release-status").textContent, /No complete stable release/);
  }
});

test("packages from another version are not mixed into a download", async () => {
  const entry = release("v0.1.6");
  entry.assets[0] = release("v0.2.0").assets[0];
  const { nodes } = await load(entry, []);
  assert.equal(nodes.get("appimage-download").href, `${repository}/releases`);
});

test("development channel excludes unrelated prereleases and published stable releases", async () => {
  const { nodes } = await load(release("v0.1.6"), [release("v0.3.0", true),
    release("v0.4.0-rc.1", true), release("v0.5.0")]);
  assert.match(nodes.get("development-release-status").textContent, /No complete development build/);
});

test("one failed channel does not prevent the other channel from rendering", async () => {
  const development = release("v0.2.0-dev.184.g233be51", true);
  for (const channel of ["stable", "development"]) {
    const { nodes } = await load(release("v0.1.6"), [development], channel);
    if (channel === "stable") {
      assert.match(nodes.get("release-status").textContent, /Couldn’t check/);
      assert.match(nodes.get("development-appimage-download").href, /\/v0\.2\.0-dev/);
    } else {
      assert.match(nodes.get("appimage-download").href, /\/v0\.1\.6\//);
      assert.match(nodes.get("development-release-status").textContent, /Couldn’t check/);
    }
  }
});

test("no releases yet keeps usable GitHub fallback links", async () => {
  const { nodes } = await load(null, []);
  assert.match(nodes.get("release-status").textContent, /No stable release published/);
  assert.equal(nodes.get("appimage-download").href, `${repository}/releases`);
  assert.equal(nodes.get("development-appimage-download").href, `${repository}/releases`);
});
