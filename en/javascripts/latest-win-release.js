/**
 * Resolve latest Windows NPR Port zip from GitHub Releases.
 * Buttons: <a data-npr-latest-win href="...fallback...">
 */
(function () {
  var REPO = "bb-yi/blender";
  var cache = null;
  var inflight = null;

  function pickWinZip(release) {
    var assets = (release && release.assets) || [];
    var i, a, name;
    // Prefer full win64 zip with npr-port
    for (i = 0; i < assets.length; i++) {
      a = assets[i];
      name = a.name || "";
      if (/npr-port-win64.*\.zip$/i.test(name)) return a.browser_download_url;
    }
    for (i = 0; i < assets.length; i++) {
      a = assets[i];
      name = a.name || "";
      if (/win64.*\.zip$/i.test(name)) return a.browser_download_url;
    }
    return null;
  }

  function fetchLatestWinZip() {
    if (cache) return Promise.resolve(cache);
    if (inflight) return inflight;
    inflight = fetch("https://api.github.com/repos/" + REPO + "/releases/latest", {
      headers: { Accept: "application/vnd.github+json" },
    })
      .then(function (r) {
        if (!r.ok) throw new Error("github " + r.status);
        return r.json();
      })
      .then(function (j) {
        var url = pickWinZip(j);
        if (!url) throw new Error("no win64 zip");
        cache = url;
        return url;
      })
      .finally(function () {
        inflight = null;
      });
    return inflight;
  }

  function rewriteButtons() {
    var nodes = document.querySelectorAll("a[data-npr-latest-win]");
    if (!nodes.length) return;
    fetchLatestWinZip()
      .then(function (url) {
        nodes.forEach(function (a) {
          a.setAttribute("href", url);
          a.setAttribute("data-npr-resolved", "1");
        });
      })
      .catch(function () {
        /* keep fallback href */
      });
  }

  document.addEventListener(
    "click",
    function (ev) {
      var a = ev.target && ev.target.closest ? ev.target.closest("a[data-npr-latest-win]") : null;
      if (!a) return;
      // If already resolved, let browser navigate
      if (a.getAttribute("data-npr-resolved") === "1") return;
      ev.preventDefault();
      var fallback = a.getAttribute("href") || "https://github.com/" + REPO + "/releases/latest";
      fetchLatestWinZip()
        .then(function (url) {
          window.location.href = url;
        })
        .catch(function () {
          window.location.href = fallback;
        });
    },
    true
  );

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", rewriteButtons);
  } else {
    rewriteButtons();
  }
})();
