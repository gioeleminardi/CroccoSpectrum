const repository = "https://github.com/gioeleminardi/CroccoSpectrum";
const releasesApi = "https://api.github.com/repos/gioeleminardi/CroccoSpectrum/releases?per_page=100";
const packages = [
  { id: "appimage", name: "AppImage", pattern: /^croccospectrum-.+-x86_64\.AppImage$/i },
  { id: "portable", name: "portable archive", pattern: /^croccospectrum-.+-x86_64\.tar\.gz$/i },
];

async function loadRelease() {
  const status = document.getElementById("release-status");
  status.textContent = "Checking the latest release…";

  try {
    const response = await fetch(releasesApi, {
      headers: { Accept: "application/vnd.github+json" },
      signal: AbortSignal.timeout(10000),
    });
    if (!response.ok) throw new Error(`GitHub returned ${response.status}`);

    // Releases can be published before CI finishes uploading their packages.
    const releases = await response.json();
    const release = releases
      .filter((entry) => !entry.draft && entry.assets.some((asset) =>
        asset.state === "uploaded" && packages.some((pkg) => pkg.pattern.test(asset.name))))
      .sort((a, b) => new Date(b.published_at) - new Date(a.published_at))[0];

    if (!release) {
      status.textContent = "No Linux packages published yet. Check GitHub Releases for updates.";
      return;
    }

    const date = new Intl.DateTimeFormat("en", {
      day: "numeric", month: "short", year: "numeric", timeZone: "UTC",
    }).format(new Date(release.published_at));
    status.textContent = `${release.tag_name} · Published ${date}`;

    const badge = document.getElementById("release-badge");
    badge.textContent = release.prerelease ? "Prerelease" : "Stable release";
    badge.hidden = false;
    document.getElementById("release-notes").href = release.html_url;

    for (const pkg of packages) {
      const asset = release.assets.find((entry) => entry.state === "uploaded" && pkg.pattern.test(entry.name));
      const link = document.getElementById(`${pkg.id}-download`);
      const label = document.getElementById(`${pkg.id}-label`);
      const meta = document.getElementById(`${pkg.id}-meta`);

      if (asset) {
        link.href = asset.browser_download_url;
        label.textContent = `Download ${pkg.name}`;
        meta.textContent = `Linux x86_64 · ${release.tag_name} · ${(asset.size / 1048576).toFixed(1)} MiB`;
      } else {
        link.href = `${repository}/releases`;
        label.textContent = `Browse ${pkg.name} releases`;
        meta.textContent = `Not included in ${release.tag_name}`;
      }
    }
  } catch {
    status.textContent = "Couldn’t check the latest version. Download from GitHub Releases below.";
  }
}

loadRelease();
