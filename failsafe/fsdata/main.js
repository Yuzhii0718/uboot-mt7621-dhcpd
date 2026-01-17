function ajax(n) {
    var t, i;
    t = window.XMLHttpRequest ? new XMLHttpRequest : new ActiveXObject("Microsoft.XMLHTTP");
    t.upload.addEventListener("progress", function (t) {
        n.progress && n.progress(t)
    });
    t.onreadystatechange = function () {
        t.readyState == 4 && t.status == 200 && n.done && n.done(t.responseText)
    };
    n.timeout && (t.timeout = n.timeout);
    i = "GET";
    n.data && (i = "POST");
    t.open(i, n.url);
    t.send(n.data)
}

function _t(n) {
    return window.fsdata && window.fsdata.t ? window.fsdata.t(n) : n
}

window.APP_STATE = window.APP_STATE || {
    page: "",
    sysinfo: null,
    backupinfo: null
};

function bytesToHuman(n) {
    var t = Number(n || 0), i, r;
    if (!t || t < 0) return "0B";
    i = ["B", "KiB", "MiB", "GiB", "TiB"];
    r = 0;
    while (t >= 1024 && r < i.length - 1) t /= 1024, r++;
    return (t >= 10 || r === 0 ? t.toFixed(0) : t.toFixed(1)) + i[r]
}

function renderSysInfo() {
    var n = document.getElementById("sysinfo"), t, i, r, u, f, e, o, s, h, c;
    if (!n) return;
    t = window.APP_STATE.sysinfo;
    if (!t) {
        n.textContent = _t("sysinfo_loading");
        return
    }
    i = t.board || {};
    r = t.ram || {};
    u = t.mtd || {};
    f = t.flash || {};
    e = [];
    o = i.model || _t("sysinfo_unknown");
    i.name && i.name !== i.model && (o += " [" + i.name + "]");
    e.push(_t("sysinfo_board") + " " + o);
    e.push(_t("sysinfo_ram") + " " + (r.size ? bytesToHuman(r.size) : _t("sysinfo_unknown")));
    /*
     * If running on NMBM, backend may provide flash.raw (physical NAND) + flash (logical NMBM).
     * Show physical NAND first, then NMBM/logical size.
     */
    if (f && f.raw && (f.raw.name || f.raw.model || f.raw.size)) {
        o = (f.raw.model ? f.raw.model : (f.raw.name || "")) + (f.raw.model && f.raw.name && f.raw.model !== f.raw.name ? " [" + f.raw.name + "]" : "") + (f.raw.size ? " (" + bytesToHuman(f.raw.size) + ")" : "");
        e.push(_t("sysinfo_flash") + " " + (o || _t("sysinfo_unknown")));
        (f.name || f.model || f.size) && (o = (f.model ? f.model : (f.name || "")) + (f.model && f.name && f.model !== f.name ? " [" + f.name + "]" : "") + (f.size ? " (" + bytesToHuman(f.size) + ")" : ""), e.push(_t("sysinfo_flash_nmbm") + " " + (o || _t("sysinfo_unknown"))));
    } else {
        (f.name || f.model || f.size) && (o = (f.model ? f.model : (f.name || "")) + (f.model && f.name && f.model !== f.name ? " [" + f.name + "]" : "") + (f.size ? " (" + bytesToHuman(f.size) + ")" : ""), e.push(_t("sysinfo_flash") + " " + (o || _t("sysinfo_unknown"))));
    }

    s = [];
    /* raw NAND is displayed in main section when available; avoid duplicating it in Details */
    i.compatible && s.push(_t("sysinfo_compat") + " " + i.compatible);
    u.parts && s.push(_t("sysinfo_mtdparts") + " " + u.parts);
    u.ids && s.push(_t("sysinfo_mtdids") + " " + u.ids);

    while (n.firstChild) n.removeChild(n.firstChild);
    h = document.createElement("div");
    h.className = "sysinfo-main";
    h.textContent = e.join("\n");
    n.appendChild(h);

    if (s.length) {
        c = document.createElement("details");
        c.className = "sysinfo-details";
        var l = document.createElement("summary");
        l.textContent = _t("sysinfo_more");
        c.appendChild(l);
        var a = document.createElement("div");
        a.className = "sysinfo-details-body";
        a.textContent = s.join("\n");
        c.appendChild(a);
        n.appendChild(c);
    }
}

function getSysInfo() {
    var n = document.getElementById("sysinfo");
    n && renderSysInfo();
    ajax({
        url: "/sysinfo",
        done: function (n) {
            try {
                window.APP_STATE.sysinfo = JSON.parse(n)
            } catch (t) {
                return
            }
            renderSysInfo()
        }
    })
}


function setBackupStatus(n) {
    var t = document.getElementById("backup_status");
    t && (t.style.display = n ? "block" : "none", t.textContent = n)
}

function setBackupProgress(n) {
    var t = document.getElementById("bar"), i;
    t && (i = Math.max(0, Math.min(100, parseInt(n || 0, 10))), t.setAttribute("style", "--percent: " + i), t.style.display = "block")
}

function backupUpdateRangeHint() {
    var n = document.getElementById("backup_range_hint"), t, i;
    if (!n) return;
    t = document.getElementById("backup_start");
    i = document.getElementById("backup_end");
    if (!t || !i) return;
    n.textContent = _t("backup_range_hint")
}

function backupInit() {
    var n = document.getElementById("backup_mode"), t = document.getElementById("backup_range"), i = document.getElementById("backup_target"), r, u;
    if (!n || !t || !i) return;

    function ensureTargetSelected() {
        var n;
        if (!i) return;
        if (i.value) return;
        /* In range mode, prefer partition targets (mtd:<part>) */
        if (document.getElementById("backup_mode") && document.getElementById("backup_mode").value === "range") {
            for (n = 0; n < i.options.length; n++) {
                if (i.options[n] && i.options[n].value && String(i.options[n].value).indexOf("mtd:") === 0) {
                    i.selectedIndex = n;
                    return;
                }
            }
        }
        for (n = 0; n < i.options.length; n++) {
            if (i.options[n] && i.options[n].value) {
                i.selectedIndex = n;
                return;
            }
        }
    }

    r = function () {
        var r = n.value === "range", u = document.getElementById("backup_target_pair");
        t.style.display = r ? "block" : "none";
        u && (u.style.display = r ? "none" : "flex");
        r && ensureTargetSelected();
        backupUpdateRangeHint()
    };
    n.onchange = r;
    u = document.getElementById("backup_start");
    u && (u.oninput = backupUpdateRangeHint);
    u = document.getElementById("backup_end");
    u && (u.oninput = backupUpdateRangeHint);
    r();
    setBackupStatus("");
    ajax({
        url: "/backupinfo",
        done: function (n) {
            var t, r, u, f;
            try {
                t = JSON.parse(n)
            } catch (e) {
                setBackupStatus("backupinfo parse failed");
                return
            }
            window.APP_STATE.backupinfo = t;
            i.options.length = 0;
            r = document.createElement("option");
            r.value = "";
            r.textContent = _t("backup_target_placeholder");
            i.appendChild(r);

            /* Full-disk options live inside partition backup target list */
            if (t.mtd && t.mtd.present && t.mtd.devices && t.mtd.devices.length) {
                t.mtd.devices.forEach(function (d) {
                    var opt, label;
                    if (!d || !d.name) return;
                    opt = document.createElement("option");
                    opt.value = "mtddev:" + d.name;
                    label = _t("backup_target_full_disk") + " " + d.name;
                    if (d.size) label += " (" + bytesToHuman(d.size) + ")";
                    opt.textContent = "[MTD] " + label;
                    i.appendChild(opt)
                })
            }

            t.mtd && t.mtd.present && t.mtd.parts && t.mtd.parts.length && t.mtd.parts.forEach(function (n) {
                var t;
                n && n.name && (t = document.createElement("option"), t.value = "mtd:" + n.name, t.textContent = "[MTD] " + n.name + (n.size ? " (" + bytesToHuman(n.size) + ")" : ""), i.appendChild(t))
            });

            /* Default to first partition target when available */
            (function () {
                var k;
                for (k = 0; k < i.options.length; k++) {
                    if (i.options[k] && i.options[k].value && String(i.options[k].value).indexOf("mtd:") === 0) {
                        i.selectedIndex = k;
                        return;
                    }
                }
                i.options.length > 1 && (i.selectedIndex = 1);
            })();
            ensureTargetSelected();
            f = document.getElementById("backup_range_hint");
            f && backupUpdateRangeHint()
        }
    })
}

function parseFilenameFromDisposition(n) {
    var t, i;
    if (!n) return "";
    t = /filename\*=UTF-8''([^;]+)|filename="?([^";]+)"?/i.exec(n);
    if (!t) return "";
    i = t[1] || t[2] || "";
    try {
        return decodeURIComponent(i)
    } catch (r) {
        return i
    }
}

function sanitizeFilenameComponent(n) {
    return n ? String(n).replace(/[^a-zA-Z0-9._-]+/g, "_").replace(/^_+|_+$/g, "").slice(0, 48) : ""
}

function getNowYYYYMMDD() {
    var n = new Date, t = n.getFullYear(), i = n.getMonth() + 1, r = n.getDate();
    return String(t) + String(i).padStart(2, "0") + String(r).padStart(2, "0")
}

function makeBackupDownloadName(n) {
    var u = (window.APP_STATE.sysinfo && window.APP_STATE.sysinfo.board && window.APP_STATE.sysinfo.board.model) ? window.APP_STATE.sysinfo.board.model : "";
    var t = sanitizeFilenameComponent(u) || "board";
    var i = getNowYYYYMMDD();
    var r = String(n || "backup.bin");

    r.indexOf("backup_") === 0 || (r = "backup_" + r.replace(/^_+/, ""));
    r.indexOf("backup_" + t + "_") === 0 || (r = r.replace(/^backup_/, "backup_" + t + "_"));
    /\.[A-Za-z0-9]+$/.test(r) || (r = r + ".bin");
    /_\d{8}\.[A-Za-z0-9]+$/.test(r) || (r = r.replace(/(\.[A-Za-z0-9]+)$/, "_" + i + "$1"));

    return r
}

async function ensureSysInfoLoaded() {
    if (window.APP_STATE.sysinfo && window.APP_STATE.sysinfo.board && window.APP_STATE.sysinfo.board.model)
        return window.APP_STATE.sysinfo;

    if (window.APP_STATE._sysinfo_promise)
        return await window.APP_STATE._sysinfo_promise;

    window.APP_STATE._sysinfo_promise = (async function () {
        try {
            var r = await fetch("/sysinfo", { method: "GET" });
            if (!r || !r.ok) return null;
            var j = await r.json();
            j && (window.APP_STATE.sysinfo = j);
            return j;
        } catch (e) {
            return null;
        } finally {
            window.APP_STATE._sysinfo_promise = null;
        }
    })();

    return await window.APP_STATE._sysinfo_promise;
}

async function startBackup() {
    var n = document.getElementById("backup_mode"), t = document.getElementById("backup_target"), i, r, u, f, e, o, s, h, c, l, a, v, y, p, w, b;
    if (!n || !t) return;
    i = n.value;
    r = t.value;
    if (!r && i === "range") {
        for (var d = 0; d < t.options.length; d++) {
            if (t.options[d] && t.options[d].value) {
                t.selectedIndex = d;
                r = t.value;
                break
            }
        }
    }
    if (!r) {
        alert(_t("backup_error_no_target"));
        return
    }
    u = new FormData;
    u.append("mode", i);
    u.append("target", r);
    if (i === "range") {
        f = document.getElementById("backup_start");
        e = document.getElementById("backup_end");
        if (!f || !e || !f.value || !e.value) {
            alert(_t("backup_error_bad_range"));
            return
        }
        u.append("start", f.value);
        u.append("end", e.value)
    }
    setBackupProgress(0);
    setBackupStatus(_t("backup_status_starting"));
    try {
        o = await fetch("/backup", { method: "POST", body: u });
        if (!o.ok) {
            setBackupStatus(_t("backup_error_http") + " " + o.status);
            return
        }
        s = o.headers.get("Content-Length");
        h = s ? parseInt(s, 10) : 0;
        c = parseFilenameFromDisposition(o.headers.get("Content-Disposition"));
        c || (c = "backup.bin");
        await ensureSysInfoLoaded();
        c = makeBackupDownloadName(c);
        l = 0;
        if (window.showSaveFilePicker) {
            a = await window.showSaveFilePicker({ suggestedName: c, types: [{ description: "Binary", accept: { "application/octet-stream": [".bin"] } }] });
            v = await a.createWritable();
            y = o.body.getReader();
            while (true) {
                p = await y.read();
                if (p.done) break;
                await v.write(p.value);
                l += p.value.length;
                h ? setBackupProgress(l / h * 100) : setBackupProgress(0);
                setBackupStatus(_t("backup_status_downloading") + " " + bytesToHuman(l) + (h ? " / " + bytesToHuman(h) : ""))
            }
            await v.close();
            setBackupProgress(100);
            setBackupStatus(_t("backup_status_done") + " " + c)
        } else {
            w = [];
            y = o.body.getReader();
            while (true) {
                p = await y.read();
                if (p.done) break;
                w.push(p.value);
                l += p.value.length;
                h ? setBackupProgress(l / h * 100) : setBackupProgress(0);
                setBackupStatus(_t("backup_status_downloading") + " " + bytesToHuman(l) + (h ? " / " + bytesToHuman(h) : ""))
            }
            setBackupProgress(100);
            setBackupStatus(_t("backup_status_preparing"));
            b = new Blob(w, { type: "application/octet-stream" });
            a = document.createElement("a");
            a.href = URL.createObjectURL(b);
            a.download = c;
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            setBackupStatus(_t("backup_status_done") + " " + c)
        }
    } catch (k) {
        setBackupStatus(_t("backup_error_exception") + " " + (k && k.message ? k.message : String(k)))
    }
}

function rebootInit() {
    var n = document.getElementById("reboot_status");
    fetch("/reboot", { method: "GET" }).then(function () {
        n && (n.innerHTML = _t("reboot_info"))
    }).catch(function () { })
}

function appInit(n) {
    window.APP_STATE.page = n || "";
    window.fsdata && window.fsdata.ensureUI && window.fsdata.ensureUI();
    getversion();
    getSysInfo();
    n === "backup" && backupInit();
    n === "reboot" && rebootInit()
}

function getversion() {
    window.fsdata && window.fsdata.ensureUI && window.fsdata.ensureUI();
    ajax({
        url: "/version",
        done: function (n) {
            var t = document.getElementById("version");
            t && (t.textContent = n);
            window.fsdata && window.fsdata.ensureUI && window.fsdata.ensureUI()
        }
    })
}

function startup() {
    window.fsdata && window.fsdata.ensureUI && window.fsdata.ensureUI();
    getversion();
    getSysInfo()
}

function upload(n) {
    var i = document.getElementById("file").files[0],
        t;
    i && (document.getElementById("form").style.display = "none", document.getElementById("hint").style.display = "none", t = new FormData, t.append(n, i), ajax({
        url: "/upload",
        data: t,
        done: function (n) {
            if (n == "fail") location = "/fail.html";
            else {
                const t = n.split(" ");
                document.getElementById("size").style.display = "block";
                document.getElementById("size").innerHTML = "Size: " + t[0];
                document.getElementById("md5").style.display = "block";
                document.getElementById("md5").innerHTML = "MD5: " + t[1];
                document.getElementById("upgrade").style.display = "block"
            }
        },
        progress: function (n) {
            var t = parseInt(n.loaded / n.total * 100);
            document.getElementById("bar").setAttribute("style", "--percent: " + t);
            document.getElementById("bar").style.display = "block"
        }
    }))
} (function () {
    function r(n, t) {
        try {
            var i = localStorage.getItem(n);
            return i || t
        } catch (r) {
            return t
        }
    }

    function s(n, t) {
        try {
            localStorage.setItem(n, t)
        } catch (i) { }
    }

    function y() {
        var n = [],
            t, i;
        for (navigator.languages && navigator.languages.length ? n = navigator.languages : navigator.language && (n = [navigator.language]), t = 0; t < n.length; t++)
            if (i = (n[t] || "").toLowerCase(), i.indexOf("zh") === 0) return "zh";
        return "en"
    }

    function h() {
        t.langPref = r(f, "auto");
        t.lang = t.langPref === "zh" || t.langPref === "en" ? t.langPref : y();
        document.documentElement.lang = t.lang
    }

    function n(n) {
        var r = i[t.lang] || i.en;
        return r && r[n] ? r[n] : i.en && i.en[n] ? i.en[n] : n
    }

    function e(t) {
        var b, e, o, s, k, c, d, g, nt, l, tt, it, i, f, rt, ut, a, ft, et, ot, st, v, y, r, p, w, u;
        for (t || (t = document), h(), b = t.querySelectorAll("[data-i18n]"), e = 0; e < b.length; e++)(o = b[e], s = o.getAttribute("data-i18n"), s) && (o.textContent = o.tagName === "TITLE" ? n(s) : n(s));
        for (k = t.querySelectorAll("[data-i18n-html]"), c = 0; c < k.length; c++)(d = k[c], g = d.getAttribute("data-i18n-html"), g) && (d.innerHTML = n(g));
        for (nt = t.querySelectorAll("[data-i18n-value]"), l = 0; l < nt.length; l++)(tt = nt[l], it = tt.getAttribute("data-i18n-value"), it) && (tt.value = n(it));
        if (i = (location.pathname || "").toLowerCase(), i === "/" || i.indexOf("index.html") >= 0 ? document.title = n("page_firmware_title") : i.indexOf("uboot.html") >= 0 ? document.title = n("page_uboot_title") : i.indexOf("initramfs.html") >= 0 ? document.title = n("page_initramfs_title") : i.indexOf("flashing.html") >= 0 ? document.title = n("page_flashing_title") : i.indexOf("booting.html") >= 0 ? document.title = n("page_booting_title") : i.indexOf("fail.html") >= 0 ? document.title = n("page_fail_title") : i.indexOf("success.html") >= 0 ? document.title = n("page_success_title") : i.indexOf("404") >= 0 && (document.title = n("page_404_title")), f = document.querySelector(".app-shell"), f) {
            for (rt = f.querySelector(".app-brand"), rt && (rt.textContent = n("brand")), ut = f.querySelectorAll("[data-nav-key]"), a = 0; a < ut.length; a++) ft = ut[a], et = ft.getAttribute("data-nav-key"), et && (ft.textContent = n(et));
            if (ot = f.querySelector('[data-i18n="ctl_language"]'), ot && (ot.textContent = n("ctl_language")), st = f.querySelector('[data-i18n="ctl_theme"]'), st && (st.textContent = n("ctl_theme")), v = document.getElementById("lang-select"), v)
                for (y = 0; y < v.options.length; y++) r = v.options[y], r.value === "auto" ? r.textContent = n("opt_auto") : r.value === "zh" ? r.textContent = n("opt_zh") : r.value === "en" && (r.textContent = n("opt_en"));
            if (p = document.getElementById("theme-select"), p)
                for (w = 0; w < p.options.length; w++) u = p.options[w], u.value === "auto" ? u.textContent = n("opt_auto") : u.value === "light" ? u.textContent = n("opt_light") : u.value === "dark" && (u.textContent = n("opt_dark"))
        }
    }

    function c() {
        t.themePref = r(u, "auto");
        var n = document.documentElement;
        t.themePref === "light" || t.themePref === "dark" ? n.setAttribute("data-theme", t.themePref) : n.removeAttribute("data-theme")
    }

    function l(n) {
        t.themePref = n || "auto";
        s(u, t.themePref);
        c();
        o()
    }

    function a(n) {
        t.langPref = n || "auto";
        s(f, t.langPref);
        e(document);
        o()
    }

    function p(n) {
        var t = (location.pathname || "/").toLowerCase(),
            i = (n || "").toLowerCase();
        return i === "/" ? t === "/" || t.indexOf("index.html") >= 0 : t.indexOf(i.replace("/", "")) >= 0
    }

    function w() {
        var g, s, h, i, nt, tt, rt, it, c, r, v, y, w, u, b, k, f, d, ut;
        if (!t.uiReady && (g = document.getElementById("m"), g)) {
            for (s = document.body, h = document.createElement("div"), h.className = "app-shell", i = document.createElement("aside"), i.className = "app-sidebar", nt = document.createElement("div"), nt.className = "app-brand", nt.textContent = n("brand"), i.appendChild(nt), tt = document.createElement("nav"), tt.className = "app-nav", rt = [{
                href: "/",
                key: "nav_firmware"
            }, {
                href: "/uboot.html",
                key: "nav_uboot"
            }, {
                href: "/initramfs.html",
                key: "nav_initramfs"
            }, {
                href: "/factory.html",
                key: "nav_factory"
            }, {
                href: "/backup.html",
                key: "nav_backup"
            }, {
                href: "/reboot.html",
                key: "nav_reboot"
            }], it = 0; it < rt.length; it++) c = rt[it], r = document.createElement("a"), r.href = c.href, r.setAttribute("data-nav-key", c.key), r.textContent = n(c.key), p(c.href) && (r.className = "active"), tt.appendChild(r);
            for (it = 0; it < tt.children.length; it++) {
                var ct = tt.children[it];
                ct && ct.getAttribute && ct.getAttribute("data-nav-key") === "nav_reboot" && (ct.onclick = function () {
                    return confirm(n("reboot_confirm"))
                })
            }
            for (i.appendChild(tt), v = document.createElement("div"), v.className = "app-controls", y = document.createElement("div"), y.className = "app-control-row", w = document.createElement("div"), w.className = "app-control-label", w.setAttribute("data-i18n", "ctl_language"), w.textContent = n("ctl_language"), u = document.createElement("select"), u.className = "app-select", u.id = "lang-select", u.innerHTML = '<option value="auto">' + n("opt_auto") + '<\/option><option value="zh">' + n("opt_zh") + '<\/option><option value="en">' + n("opt_en") + "<\/option>", u.addEventListener("change", function () {
                a(this.value)
            }), y.appendChild(w), y.appendChild(u), v.appendChild(y), b = document.createElement("div"), b.className = "app-control-row", k = document.createElement("div"), k.className = "app-control-label", k.setAttribute("data-i18n", "ctl_theme"), k.textContent = n("ctl_theme"), f = document.createElement("select"), f.className = "app-select", f.id = "theme-select", f.innerHTML = '<option value="auto">' + n("opt_auto") + '<\/option><option value="light">' + n("opt_light") + '<\/option><option value="dark">' + n("opt_dark") + "<\/option>", f.addEventListener("change", function () {
                l(this.value)
            }), b.appendChild(k), b.appendChild(f), v.appendChild(b), i.appendChild(v), d = document.createElement("main"), d.className = "app-content", g.classList.add("app-card"), d.appendChild(g), ut = document.getElementById("version"), ut && d.appendChild(ut), h.appendChild(i), h.appendChild(d); s.firstChild;) s.removeChild(s.firstChild);
            s.appendChild(h);
            t.uiReady = !0;
            o();
            e(document)
        }
    }

    function o() {
        var t = document.getElementById("lang-select"),
            n;
        t && (t.value = r(f, "auto"));
        n = document.getElementById("theme-select");
        n && (n.value = r(u, "auto"))
    }

    function b() {
        for (var n, u, t, r = document.querySelectorAll("div#version"), i = 0; i < r.length; i++)(n = r[i], n) && ((u = n.querySelector(".yuzhii-tag"), u) || (t = document.createElement("span"), t.className = "yuzhii-tag", t.textContent = "💡Yuzhii", n.textContent && n.textContent.trim().length > 0 && n.appendChild(document.createTextNode(" ")), n.appendChild(t)))
    }

    function v() {
        c();
        h();
        w();
        b()
    }
    var u = "fsdata_theme",
        f = "fsdata_lang",
        t = {
            uiReady: !1,
            themePref: "auto",
            langPref: "auto",
            lang: "en"
        },
        i = {
            en: {
                brand: "System Recovery Mode",
                nav_firmware: "Firmware upgrade",
                nav_uboot: "U-Boot update",
                nav_initramfs: "Initramfs boot",
                nav_factory: "Factory update",
                nav_backup: "Backup",
                nav_reboot: "Reboot",
                ctl_language: "🌐 Language",
                ctl_theme: "🎨 Theme",
                opt_auto: "Auto",
                opt_light: "Light",
                opt_dark: "Dark",
                opt_zh: "中文",
                opt_en: "English",
                page_firmware_title: "Firmware update",
                h_firmware: "FIRMWARE UPDATE",
                hint_firmware: "You are going to update <strong>firmware<\/strong> on the device.<br>Please, choose file from your local hard drive and click <strong>Upload<\/strong> button.",
                other_warnings: "WARNINGS",
                warn_no_poweroff: "do not power off the device during update",
                warn_restart: "if everything goes well, the device will restart",
                warn_choose_fw: "you can upload whatever you want, so be sure that you choose proper firmware image for your device",
                btn_upload: "Upload",
                prompt_update: 'If all information above is correct, click "Update".',
                btn_update: "Update",
                page_uboot_title: "U-Boot update",
                h_uboot: "U-BOOT UPDATE",
                hint_uboot: "You are going to update <strong>U-Boot (bootloader)<\/strong> on the device.<br>Please, choose file from your local hard drive and click <strong>Upload<\/strong> button.",
                warn_choose_uboot: "you can upload whatever you want, so be sure that you choose proper U-Boot image for your device",
                warn_uboot_danger: "updating U-Boot is a very dangerous operation and may damage your device!",
                page_initramfs_title: "Load initramfs",
                h_initramfs: "BOOT INITRAMFS",
                hint_initramfs: "You are going to boot <strong>initramfs<\/strong> on the device.<br>Please, choose file from your local hard drive and click <strong>Upload<\/strong> button.",
                warn_boot_initramfs: "if everything goes well, the device will boot into initramfs",
                warn_choose_initramfs: "you can upload whatever you want, so be sure that you choose proper initramfs image for your device",
                prompt_boot: 'If all information above is correct, click "Boot".',
                btn_boot: "Boot",
                page_flashing_title: "Update in progress",
                h_update_in_progress: "UPDATE IN PROGRESS",
                p_update_in_progress: "Your file was successfully uploaded! Update is in progress and you should wait for automatic reset of the device.<br>Update time depends on image size and may take up to a few minutes.",
                h_update_completed: "UPDATE COMPLETED",
                p_update_completed: "Your device was successfully updated! Now rebooting...",
                page_booting_title: "Booting initramfs",
                h_booting_initramfs: "BOOTING INITRAMFS",
                p_booting_initramfs: "Your file was successfully uploaded! Booting is in progress, please wait...<br>This page may be in not responding status for a short time.",
                h_boot_success: "BOOT SUCCESS",
                p_boot_success: "Your device was successfully booted into initramfs!",
                page_fail_title: "Update failed",
                h_update_failed: "UPDATE FAILED",
                fail_detail_html: "<strong>Something went wrong during update<\/strong>Probably you have chosen wrong file. Please, try again. You can also get more information during update in U-Boot console.",
                page_success_title: "Success",
                h_success: "SUCCESS",
                p_success: "OK",
                page_404_title: "404 - Not found",
                page_not_found: "Page not found",
                page_factory_title: "Factory update",
                h_factory: "FACTORY UPDATE",
                hint_factory: "You are going to update <strong>Factory (Wireless Calibration)<\/strong> partition on the device.<br>Please, choose file from your local hard drive and click <strong>Upload<\/strong> button.",
                warn_factory_danger: "updating factory partition may damage your device or break calibration data",
                page_backup_title: "Backup",
                h_backup: "BACKUP",
                hint_backup: "Download a backup from device storage as a <strong>binary file<\/strong>.<br>The backup data will be streamed to your browser and saved on your computer.",
                backup_label_mode: "Mode:",
                backup_label_target: "Target:",
                backup_label_start: "Start:",
                backup_label_end: "End (exclusive):",
                backup_mode_part: "Partition backup",
                backup_mode_range: "Custom range",
                backup_mode_full: "Full disk backup",
                backup_btn_download: "Download",
                backup_target_placeholder: "-- select --",
                backup_range_hint: "Tip: input supports decimal, 0xHEX, and KiB suffix (e.g. 64KiB).",
                backup_warn_1: "do not power off the device during backup",
                backup_warn_2: "custom range reads raw bytes; be careful with offsets",
                backup_warn_3: "large backups may take a long time depending on storage speed",
                backup_status_starting: "Starting...",
                backup_status_downloading: "Downloading:",
                backup_status_preparing: "Preparing file...",
                backup_status_done: "Done:",
                backup_error_no_target: "Please select a target",
                backup_error_bad_range: "Please input valid start/end",
                backup_error_http: "HTTP error",
                backup_error_exception: "Error:",
                backup_target_full_disk: "Full disk",
                page_reboot_title: "Reboot",
                h_reboot: "REBOOTING DEVICE",
                reboot_info: "Reboot request has been sent. Please wait...<br>This page may be in not responding status for a short time.",
                reboot_warn_1: "do not power off the device during reboot",
                reboot_confirm: "Reboot device now?",
                sysinfo_loading: "Loading system info...",
                sysinfo_board: "Board:",
                sysinfo_ram: "RAM:",
                sysinfo_unknown: "unknown",
                sysinfo_compat: "Compatible:",
                sysinfo_flash: "Flash:",
                sysinfo_flash_raw: "Raw:",
                sysinfo_flash_nmbm: "NMBM:",
                sysinfo_mtdparts: "mtdparts:",
                sysinfo_mtdids: "mtdids:",
                sysinfo_more: "Details"
            },
            zh: {
                brand: "系统恢复模式",
                nav_firmware: "固件升级",
                nav_uboot: "U-Boot 更新",
                nav_initramfs: "Initramfs 启动",
                nav_factory: "Factory 更新",
                nav_backup: "备份",
                nav_reboot: "重启",
                ctl_language: "🌐 语言",
                ctl_theme: "🎨 主题",
                opt_auto: "自动",
                opt_light: "浅色",
                opt_dark: "深色",
                opt_zh: "中文",
                opt_en: "English",
                page_firmware_title: "固件升级",
                h_firmware: "固件升级",
                hint_firmware: "你将要为设备更新<strong>固件<\/strong>。<br>请选择本地文件并点击<strong>上传<\/strong>按钮。",
                other_warnings: "注意事项",
                warn_no_poweroff: "升级过程中请勿断电",
                warn_restart: "如果一切正常，设备将会重启",
                warn_choose_fw: "你可以上传任意文件，请确认选择了适配你设备的正确固件镜像",
                btn_upload: "上传",
                prompt_update: "确认以上信息无误后，点击“更新”。",
                btn_update: "更新",
                page_uboot_title: "U-Boot 更新",
                h_uboot: "U-Boot 更新",
                hint_uboot: "你将要为设备更新<strong>U-Boot（引导程序）<\/strong>。<br>请选择本地文件并点击<strong>上传<\/strong>按钮。",
                warn_choose_uboot: "你可以上传任意文件，请确认选择了适配你设备的正确 U-Boot 镜像",
                warn_uboot_danger: "更新 U-Boot 风险极高，可能导致设备损坏！",
                page_initramfs_title: "加载 initramfs",
                h_initramfs: "启动 Initramfs",
                hint_initramfs: "你将要为设备启动<strong>initramfs<\/strong>。<br>请选择本地文件并点击<strong>上传<\/strong>按钮。",
                warn_boot_initramfs: "如果一切正常，设备将启动进入 initramfs",
                warn_choose_initramfs: "你可以上传任意文件，请确认选择了适配你设备的正确 initramfs 镜像",
                prompt_boot: "确认以上信息无误后，点击“启动”。",
                btn_boot: "启动",
                page_flashing_title: "正在更新",
                h_update_in_progress: "更新进行中",
                p_update_in_progress: "文件已成功上传！正在更新中，请等待设备自动重启。<br>更新时间取决于镜像大小，可能需要几分钟。",
                h_update_completed: "更新完成",
                p_update_completed: "设备已成功更新！正在重启...",
                page_booting_title: "正在启动 initramfs",
                h_booting_initramfs: "正在启动 Initramfs",
                p_booting_initramfs: "文件已成功上传！正在启动中，请稍候...<br>此页面可能会短暂无响应。",
                h_boot_success: "启动成功",
                p_boot_success: "设备已成功启动进入 initramfs！",
                page_fail_title: "更新失败",
                h_update_failed: "更新失败",
                fail_detail_html: "<strong>更新过程中出现问题<\/strong>可能是你选择了错误的文件。请重试。你也可以在 U-Boot 控制台查看更详细的更新日志。",
                page_success_title: "成功",
                h_success: "成功",
                p_success: "操作完成",
                page_404_title: "404 - 未找到页面",
                page_not_found: "页面不存在",
                page_factory_title: "Factory 更新",
                h_factory: "Factory 更新",
                hint_factory: "你将要为设备更新<strong>Factory（无线校准）<\/strong>分区。<br>请选择本地文件并点击<strong>上传<\/strong>按钮。",
                warn_factory_danger: "更新 Factory 分区可能导致设备损坏或校准数据丢失",
                page_backup_title: "备份",
                h_backup: "备份",
                hint_backup: "从设备存储下载<strong>二进制文件<\/strong>备份。<br>备份数据将以流式方式下载并保存到你的电脑。",
                backup_label_mode: "模式：",
                backup_label_target: "目标：",
                backup_label_start: "起始：",
                backup_label_end: "结束（不包含）：",
                backup_mode_part: "按分区备份",
                backup_mode_range: "自定义范围",
                backup_mode_full: "全盘备份",
                backup_btn_download: "下载",
                backup_target_placeholder: "-- 请选择 --",
                backup_range_hint: "提示：支持十进制、0xHEX、KiB 后缀（例如 64KiB）。",
                backup_warn_1: "备份过程中请勿断电",
                backup_warn_2: "自定义范围按原始字节读取，请谨慎设置偏移",
                backup_warn_3: "大容量备份可能耗时较长，取决于闪存速度",
                backup_status_starting: "正在开始...",
                backup_status_downloading: "下载中：",
                backup_status_preparing: "正在生成文件...",
                backup_status_done: "完成：",
                backup_error_no_target: "请选择目标",
                backup_error_bad_range: "请输入有效的起始/结束",
                backup_error_http: "HTTP 错误",
                backup_error_exception: "错误：",
                backup_target_full_disk: "全盘",
                page_reboot_title: "重启",
                h_reboot: "设备正在重启",
                reboot_info: "已发送重启请求，请稍候...<br>此页面可能会短暂无响应。",
                reboot_warn_1: "重启过程中请勿断电",
                reboot_confirm: "现在重启设备？",
                sysinfo_loading: "正在获取系统信息...",
                sysinfo_board: "板型：",
                sysinfo_ram: "内存：",
                sysinfo_unknown: "未知",
                sysinfo_compat: "兼容：",
                sysinfo_flash: "闪存：",
                sysinfo_flash_raw: "原始：",
                sysinfo_flash_nmbm: "NMBM（逻辑）：",
                sysinfo_mtdparts: "分区表(mtdparts)：",
                sysinfo_mtdids: "设备映射(mtdids)：",
                sysinfo_more: "更多信息"
            }
        };
    window.fsdata = {
        t: n,
        applyI18n: e,
        ensureUI: v,
        setThemePref: l,
        setLangPref: a
    };
    document.addEventListener("DOMContentLoaded", function () {
        v()
    })
})()