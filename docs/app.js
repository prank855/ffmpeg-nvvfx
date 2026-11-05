(function () {
    "use strict";

    var G = window.NVVFXGen;
    var $ = function (id) { return document.getElementById(id); };
    var commandEl = $("command");
    var noteEl = $("note");
    var copyBtn = $("copy");

    var presets = {
        "8bit": {
            mode: "superres", quality: "ultra", scale: "2", bitDepth: "8",
            encoder: "av1_nvenc", decode: "d3d11", artifactQuality: "conservative"
        },
        "10bit": {
            mode: "superres", quality: "ultra", scale: "2", bitDepth: "10",
            encoder: "av1_nvenc", decode: "d3d11"
        },
        "denoise": {
            mode: "denoise", quality: "high", bitDepth: "8",
            encoder: "av1_nvenc", decode: "d3d11"
        },
        "artifact": {
            mode: "artifact", artifactQuality: "conservative",
            encoder: "av1_nvenc", decode: "d3d11"
        },
        "chain": {
            mode: "artifact+superres", artifactQuality: "conservative",
            quality: "ultra", scale: "2", encoder: "av1_nvenc", decode: "d3d11"
        },
        "hdr": {
            mode: "hdr", encoder: "hevc_nvenc", decode: "d3d11"
        }
    };

    function value(id) { return $(id).value; }
    function checked(id) { return $(id).checked; }

    function readState() {
        return {
            input: value("input"),
            output: value("output"),
            libDir: value("libDir"),
            shell: value("shell"),
            mode: value("mode"),
            quality: value("quality"),
            artifactQuality: value("artifactQuality"),
            scale: value("scale"),
            bitDepth: value("bitDepth"),
            strength: value("strength"),
            decode: value("decode"),
            encoder: value("encoder"),
            preset: value("preset"),
            bitrate: value("bitrate"),
            duration: value("duration"),
            copyAudio: checked("copyAudio"),
            copySubs: checked("copySubs"),
            copyAttachments: checked("copyAttachments"),
            overwrite: checked("overwrite")
        };
    }

    function setValue(id, v) { $(id).value = v; }
    function setChecked(id, v) { $(id).checked = !!v; }

    function applyPreset(name) {
        var p = presets[name];
        if (!p)
            return;
        Object.keys(p).forEach(function (key) { setValue(key, p[key]); });
        update();
    }

    function update() {
        var s = readState();
        var vsr = s.mode === "superres" || s.mode === "denoise" || s.mode === "deblur";
        var chain = s.mode === "artifact+superres";
        var artifact = s.mode === "artifact" || chain;
        var hdr = s.mode === "hdr";

        ["[data-show='vsr']", "[data-show='artifact']", "[data-show='scale']",
         "[data-show='depth']", "[data-show='strength']"].forEach(function (sel) {
            document.querySelectorAll(sel).forEach(function (el) {
                el.classList.add("is-hidden");
            });
        });

        var show = function (name, on) {
            document.querySelectorAll("[data-show='" + name + "']").forEach(function (el) {
                el.classList.toggle("is-hidden", !on);
            });
        };

        show("vsr", vsr || chain);
        show("artifact", artifact);
        show("scale", s.mode === "superres" || chain);
        show("depth", vsr);
        show("strength", vsr || chain);

        var tenBit = vsr && s.bitDepth === "10";
        var forceTen = hdr || tenBit;
        var enc = $("encoder");
        var h264 = enc.querySelector("option[value='h264_nvenc']");
        h264.disabled = forceTen;
        if (forceTen && enc.value === "h264_nvenc")
            enc.value = "hevc_nvenc";

        var note = "";
        if (hdr) {
            note = "HDR mode outputs x2bgr10le; the filter chain converts it to P010 HDR10. Use HEVC or AV1.";
        } else if (tenBit) {
            note = "True 10-bit requires P010 or X2BGR10 input. Output is P010.";
        } else if (chain) {
            note = "Artifact reduction runs first. This chain is 8-bit only.";
        } else if (artifact) {
            note = "Artifact reduction is limited to 1920x1080 and does not need the wheel.";
        } else if (s.decode === "nvdec") {
            note = "NVDEC decodes on the GPU, then uploads to D3D11. D3D11VA is the zero-copy path.";
        }
        noteEl.textContent = note;

        commandEl.textContent = G.buildCommand(s);
    }

    $("builder").addEventListener("input", update);
    $("builder").addEventListener("change", update);

    document.querySelectorAll("[data-preset]").forEach(function (btn) {
        btn.addEventListener("click", function () {
            applyPreset(btn.getAttribute("data-preset"));
        });
    });

    copyBtn.addEventListener("click", function () {
        var text = commandEl.textContent;
        var done = function () {
            var old = copyBtn.textContent;
            copyBtn.textContent = "Copied";
            setTimeout(function () { copyBtn.textContent = old; }, 1200);
        };
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(text).then(done).catch(function () {});
        } else {
            var area = document.createElement("textarea");
            area.value = text;
            document.body.appendChild(area);
            area.select();
            try { document.execCommand("copy"); done(); } catch (e) {}
            document.body.removeChild(area);
        }
    });

    update();
}());
