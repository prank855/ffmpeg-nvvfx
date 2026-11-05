(function () {
    "use strict";

    var G = window.NVVFXGen;
    var $ = function (id) { return document.getElementById(id); };
    var chainEl = $("chain");
    var commandEl = $("command");
    var noteEl = $("note");
    var copyBtn = $("copy");

    var EFFECT_LABELS = {
        superres: "Super resolution",
        denoise: "Denoise",
        deblur: "Deblur",
        artifact: "Artifact reduction",
        hdr: "SDR → HDR10"
    };

    var PRESETS = {
        "8bit": {
            chain: [{ effect: "superres", quality: "ultra", scale: "2", strength: "" }],
            inputDepth: "8", processDepth: "8", encoder: "av1_nvenc", decode: "d3d11"
        },
        "10bit": {
            chain: [{ effect: "superres", quality: "ultra", scale: "2", strength: "" }],
            inputDepth: "10", processDepth: "10", encoder: "av1_nvenc", decode: "d3d11"
        },
        "8to10": {
            chain: [{ effect: "superres", quality: "ultra", scale: "2", strength: "" }],
            inputDepth: "8", processDepth: "10", encoder: "av1_nvenc", decode: "d3d11"
        },
        "denoise": {
            chain: [{ effect: "denoise", quality: "high", scale: "2", strength: "" }],
            inputDepth: "8", processDepth: "8", encoder: "av1_nvenc", decode: "d3d11"
        },
        "artifact": {
            chain: [{ effect: "artifact", quality: "conservative", scale: "2", strength: "" }],
            inputDepth: "8", processDepth: "8", encoder: "av1_nvenc", decode: "d3d11"
        },
        "chain": {
            chain: [
                { effect: "artifact", quality: "conservative", scale: "2", strength: "" },
                { effect: "superres", quality: "ultra", scale: "2", strength: "" }
            ],
            inputDepth: "8", processDepth: "8", encoder: "av1_nvenc", decode: "d3d11"
        },
        "hdr": {
            chain: [{ effect: "hdr", quality: "high", scale: "2", strength: "" }],
            inputDepth: "8", processDepth: "10", encoder: "hevc_nvenc", decode: "d3d11"
        }
    };

    function option(value, label, selected) {
        var o = document.createElement("option");
        o.value = value;
        o.textContent = label;
        if (selected)
            o.selected = true;
        return o;
    }

    function makeButton(text, title) {
        var b = document.createElement("button");
        b.type = "button";
        b.className = "secondary small";
        b.textContent = text;
        b.title = title;
        return b;
    }

    function syncRow(row, preferredQuality) {
        var effect = row.querySelector(".effect-type").value;
        var quality = row.querySelector(".effect-quality");
        var scale = row.querySelector(".effect-scale");
        var strength = row.querySelector(".effect-strength");
        var current = preferredQuality || quality.value;
        var values = [];

        if (effect === "artifact")
            values = G.artifactQuality.slice();
        else if (G.vsrEffects.indexOf(effect) >= 0)
            values = G.vsrQuality.slice();

        quality.innerHTML = "";
        if (!values.length) {
            quality.appendChild(option("high", "—", true));
            quality.disabled = true;
        } else {
            values.forEach(function (q) {
                quality.appendChild(option(q, q, q === current));
            });
            if (values.indexOf(current) < 0)
                quality.value = values[0];
            quality.disabled = false;
        }

        scale.classList.toggle("is-hidden", effect !== "superres");
        strength.classList.toggle("is-hidden", G.vsrEffects.indexOf(effect) < 0);
    }

    function makeRow(entry) {
        var row = document.createElement("div");
        row.className = "effect-row";

        var type = document.createElement("select");
        type.className = "effect-type";
        G.effects.forEach(function (e) {
            type.appendChild(option(e, EFFECT_LABELS[e], e === entry.effect));
        });

        var quality = document.createElement("select");
        quality.className = "effect-quality";

        var scale = document.createElement("select");
        scale.className = "effect-scale";
        ["2", "3", "4"].forEach(function (v) {
            scale.appendChild(option(v, v + "x", v === String(entry.scale)));
        });

        var strength = document.createElement("input");
        strength.className = "effect-strength";
        strength.type = "number";
        strength.min = "0";
        strength.max = "1";
        strength.step = "0.05";
        strength.placeholder = "1.0";
        strength.value = entry.strength || "";

        var buttons = document.createElement("div");
        buttons.className = "effect-buttons";
        var up = makeButton("↑", "Move up");
        var down = makeButton("↓", "Move down");
        var remove = makeButton("×", "Remove");
        buttons.appendChild(up);
        buttons.appendChild(down);
        buttons.appendChild(remove);

        row.appendChild(type);
        row.appendChild(quality);
        row.appendChild(scale);
        row.appendChild(strength);
        row.appendChild(buttons);

        syncRow(row, entry.quality);

        type.addEventListener("change", function () { syncRow(row); update(); });
        quality.addEventListener("change", update);
        scale.addEventListener("change", update);
        strength.addEventListener("input", update);

        up.addEventListener("click", function () {
            var prev = row.previousElementSibling;
            if (prev)
                chainEl.insertBefore(row, prev);
            update();
        });
        down.addEventListener("click", function () {
            var next = row.nextElementSibling;
            if (next)
                chainEl.insertBefore(next, row);
            update();
        });
        remove.addEventListener("click", function () {
            if (chainEl.children.length > 1)
                row.remove();
            update();
        });

        return row;
    }

    function renderChain(chain) {
        chainEl.innerHTML = "";
        chain.forEach(function (entry) {
            chainEl.appendChild(makeRow(entry));
        });
    }

    function readChain() {
        return Array.prototype.map.call(chainEl.querySelectorAll(".effect-row"), function (row) {
            return {
                effect: row.querySelector(".effect-type").value,
                quality: row.querySelector(".effect-quality").value,
                scale: row.querySelector(".effect-scale").value,
                strength: row.querySelector(".effect-strength").value
            };
        });
    }

    function value(id) { return $(id).value; }
    function checked(id) { return $(id).checked; }

    function readState() {
        return {
            input: value("input"),
            output: value("output"),
            libDir: value("libDir"),
            shell: value("shell"),
            chain: readChain(),
            inputDepth: value("inputDepth"),
            processDepth: value("processDepth"),
            decode: value("decode"),
            encoder: value("encoder"),
            preset: value("preset"),
            tune: value("tune"),
            rc: value("rc"),
            bitrate: value("bitrate"),
            cq: value("cq"),
            qp: value("qp"),
            multipass: value("multipass"),
            lookahead: value("lookahead"),
            spatialAq: checked("spatialAq"),
            temporalAq: checked("temporalAq"),
            bf: value("bf"),
            gop: value("gop"),
            duration: value("duration"),
            copyAudio: checked("copyAudio"),
            copySubs: checked("copySubs"),
            copyAttachments: checked("copyAttachments"),
            overwrite: checked("overwrite")
        };
    }

    function update() {
        var chain = readChain();
        var hasHDR = chain.some(function (e) { return e.effect === "hdr"; });
        var hasArtifact = chain.some(function (e) { return e.effect === "artifact"; });
        var hasDenoise = chain.some(function (e) { return e.effect === "denoise"; });
        var vsrStages = chain.filter(function (e) {
            return G.vsrEffects.indexOf(e.effect) >= 0;
        });
        var inputDepth = value("inputDepth");
        var processDepth = value("processDepth");
        var force10 = hasHDR ||
            (processDepth === "10" && vsrStages.length > 0 && !hasArtifact);

        var enc = $("encoder");
        var h264 = enc.querySelector("option[value='h264_nvenc']");
        h264.disabled = force10;
        if (force10 && enc.value === "h264_nvenc")
            enc.value = "hevc_nvenc";

        document.querySelectorAll("[data-show='depth']").forEach(function (el) {
            el.classList.toggle("is-hidden", vsrStages.length === 0);
        });

        document.querySelectorAll("[data-rc]").forEach(function (el) {
            var kind = el.getAttribute("data-rc");
            var rc = value("rc");
            el.classList.toggle("is-hidden",
                !((kind === "bitrate" && (rc === "vbr" || rc === "cbr")) ||
                  (kind === "cq" && rc === "cq") ||
                  (kind === "qp" && rc === "constqp")));
        });

        var notes = [];
        if (hasHDR)
            notes.push("HDR output is X2BGR10; the chain converts it to P010 HDR10. Use HEVC or AV1.");
        if (processDepth === "10" && vsrStages.length && !hasArtifact) {
            if (inputDepth === "8")
                notes.push("8-bit source will be promoted to P010 before 10-bit VSR. Output is 10-bit; source detail stays 8-bit.");
            else
                notes.push("True 10-bit input → RGB10A2 VSR → P010.");
        } else if (processDepth === "8" && inputDepth === "10") {
            notes.push("10-bit input will be downconverted for 8-bit processing.");
        }
        if (hasArtifact && processDepth === "10")
            notes.push("Artifact reduction is 8-bit, so 10-bit processing is disabled for this chain.");
        if (hasArtifact && hasDenoise)
            notes.push("Artifact + denoise in one chain is not recommended.");
        if (chain.length > 1)
            notes.push("Chain order is preserved; HDR is forced last.");
        if (value("decode") === "nvdec")
            notes.push("NVDEC path is not zero-copy.");
        noteEl.textContent = notes.join(" ");

        var state = readState();
        state.encoder = enc.value;
        commandEl.textContent = G.buildCommand(state);
    }

    function applyPreset(name) {
        var p = PRESETS[name];
        if (!p)
            return;
        renderChain(p.chain);
        ["inputDepth", "processDepth", "encoder", "decode"].forEach(function (id) {
            if (p[id] != null)
                $(id).value = p[id];
        });
        update();
    }

    $("builder").addEventListener("input", update);
    $("builder").addEventListener("change", update);

    $("addEffect").addEventListener("click", function () {
        var chain = readChain();
        var entry = { effect: "superres", quality: "ultra", scale: "2", strength: "" };
        var hdrIndex = chain.findIndex(function (e) { return e.effect === "hdr"; });
        if (hdrIndex >= 0)
            chain.splice(hdrIndex, 0, entry);
        else
            chain.push(entry);
        renderChain(chain);
        update();
    });

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

    renderChain(G.defaults.chain);
    update();
}());
