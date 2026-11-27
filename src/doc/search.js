// The documentation's search (gcon doc -build): headings matched as
// you type, from search-index.js (GCONDOC: [heading, url, page]).
(function () {
  var input = document.getElementById("search");
  var list = document.getElementById("results");
  var root = document.body.getAttribute("data-root") || "";
  if (!input || !list || typeof GCONDOC === "undefined") return;
  var current = -1;

  function show() {
    var q = input.value.trim().toLowerCase();
    list.textContent = "";
    current = -1;
    if (!q) { list.hidden = true; return; }
    var hits = [];
    for (var i = 0; i < GCONDOC.length; i++) {
      var h = GCONDOC[i], t = h[0].toLowerCase(), at = t.indexOf(q);
      if (at < 0) continue;
      hits.push([at === 0 ? 0 : 1, t.length, h]);
    }
    hits.sort(function (a, b) { return a[0] - b[0] || a[1] - b[1]; });
    hits = hits.slice(0, 20);
    if (!hits.length) { list.hidden = true; return; }
    hits.forEach(function (x) {
      var li = document.createElement("li"), a = document.createElement("a");
      a.href = root + x[2][1];
      a.textContent = x[2][0];
      if (x[2][2] !== x[2][0]) {
        var p = document.createElement("span");
        p.className = "page";
        p.textContent = x[2][2];
        a.appendChild(p);
      }
      li.appendChild(a);
      list.appendChild(li);
    });
    list.hidden = false;
  }

  function move(d) {
    var links = list.querySelectorAll("a");
    if (!links.length) return;
    if (current >= 0) links[current].classList.remove("current");
    current = (current + d + links.length) % links.length;
    links[current].classList.add("current");
    links[current].scrollIntoView({ block: "nearest" });
  }

  input.addEventListener("input", show);
  input.addEventListener("keydown", function (e) {
    if (e.key === "ArrowDown") { move(1); e.preventDefault(); }
    else if (e.key === "ArrowUp") { move(-1); e.preventDefault(); }
    else if (e.key === "Enter") {
      var links = list.querySelectorAll("a");
      if (links.length) location.href = links[current >= 0 ? current : 0].href;
    } else if (e.key === "Escape") { input.value = ""; show(); input.blur(); }
  });
  document.addEventListener("keydown", function (e) {
    if (e.key === "/" && document.activeElement !== input) { input.focus(); e.preventDefault(); }
  });
  document.addEventListener("click", function (e) {
    if (e.target !== input && !list.contains(e.target)) list.hidden = true;
  });
})();
