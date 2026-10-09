const repository = "https://github.com/gioeleminardi/CroccoSpectrum";
const releasesApi = "https://api.github.com/repos/gioeleminardi/CroccoSpectrum/releases";
const packages = [
  { id: "appimage", name: "AppImage", suffix: "-x86_64.AppImage" },
  { id: "portable", name: "portable archive", suffix: "-x86_64.tar.gz" },
];
const stableTag = /^v?(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$/;
const developmentTag = /^v((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)-dev\.([1-9][0-9]*)\.g([0-9a-f]{7}))$/;

function releaseAssets(release) {
  if (!release || release.draft !== false || !Array.isArray(release.assets)) return null;
  const version = release.tag_name.replace(/^v/, "");
  const uploaded = new Map(release.assets
    .filter((asset) => asset.state === "uploaded" && asset.size > 0)
    .map((asset) => [asset.name, asset]));
  const assets = packages.map((pkg) => uploaded.get(`croccospectrum-${version}${pkg.suffix}`));
  if (assets.some((asset) => !asset || !uploaded.has(`${asset.name}.sha256`))) return null;
  if (release.prerelease && ["BUILD-INFO.json", `croccospectrum-${version}-source.tar.gz`,
    `croccospectrum-${version}-source.tar.gz.sha256`, "dependency-sources.tar.gz",
    "dependency-sources.tar.gz.sha256"].some((name) => !uploaded.has(name))) return null;
  return assets;
}

function selectDevelopmentRelease(releases) {
  return releases
    .filter((release) => release.prerelease === true && developmentTag.test(release.tag_name)
      && releaseAssets(release))
    .sort((a, b) => Number(developmentTag.exec(b.tag_name)[2])
      - Number(developmentTag.exec(a.tag_name)[2]))[0];
}

async function loadRelease(development = false) {
  const prefix = development ? "development-" : "";
  const channel = development ? "development build" : "stable release";
  const status = document.getElementById(`${prefix}release-status`);
  status.textContent = `Checking the latest ${channel}…`;

  try {
    const response = await fetch(`${releasesApi}${development ? "?per_page=100" : "/latest"}`, {
      headers: { Accept: "application/vnd.github+json" },
      signal: AbortSignal.timeout(10000),
    });
    if (response.status === 404) {
      status.textContent = `No ${channel} published yet. Check GitHub Releases for updates.`;
      return;
    }
    if (!response.ok) throw new Error(`GitHub returned ${response.status}`);
    const data = await response.json();
    const release = development ? selectDevelopmentRelease(data) : data;
    if (!release || (!development && (release.prerelease !== false || !stableTag.test(release.tag_name)))
      || !releaseAssets(release)) {
      status.textContent = `No complete ${channel} available yet. Check GitHub Releases for updates.`;
      return;
    }

    const buildDate = /^Build date: (.+)$/m.exec(release.body || "")?.[1];
    const date = new Intl.DateTimeFormat("en", {
      day: "numeric", month: "short", year: "numeric", timeZone: "UTC",
    }).format(new Date(development && buildDate ? buildDate : release.published_at));
    const match = development ? developmentTag.exec(release.tag_name) : null;
    status.textContent = `${release.tag_name} · ${development ? "Built" : "Published"} ${date}`
      + (match ? ` · Commit ${match[3]}` : "");

    const badge = document.getElementById(`${prefix}release-badge`);
    badge.textContent = development ? "Development" : "Stable release";
    badge.hidden = false;
    document.getElementById(`${prefix}release-notes`).href = release.html_url;

    const assets = releaseAssets(release);
    for (const [index, pkg] of packages.entries()) {
      const asset = assets[index];
      document.getElementById(`${prefix}${pkg.id}-download`).href = asset.browser_download_url;
      document.getElementById(`${prefix}${pkg.id}-label`).textContent = `Download ${pkg.name}`;
      document.getElementById(`${prefix}${pkg.id}-meta`).textContent =
        `Linux x86_64 · ${release.tag_name} · ${(asset.size / 1048576).toFixed(1)} MiB`;
    }
  } catch {
    status.textContent = `Couldn’t check the latest ${channel}. Download from GitHub Releases below.`;
  }
}

loadRelease();
loadRelease(true);
