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
        denoise: "Denoise (VSR)",
        deblur: "Deblur",
        artifact: "Artifact reduction",
        hdr: "SDR → HDR10"
    };

    var PRESETS = {
        "8bit": {
            tool: "vfx",
            chain: [{ effect: "superres", quality: "ultra", scale: "2", strength: "" }],
            inputDepth: "8", processDepth: "8", encoder: "av1_nvenc", decode: "d3d11"
        },
        "10bit": {
            tool: "vfx",
            chain: [{ effect: "superres", quality: "ultra", scale: "2", strength: "" }],
            inputDepth: "10", processDepth: "10", encoder: "av1_nvenc", decode: "d3d11"
        },
        "8to10": {
            tool: "vfx",
            chain: [{ effect: "superres", quality: "ultra", scale: "2", strength: "" }],
            inputDepth: "8", processDepth: "10", encoder: "av1_nvenc", decode: "d3d11"
        },
        "denoise": {
            tool: "vfx",
            chain: [{ effect: "denoise", quality: "high", scale: "2", strength: "" }],
            inputDepth: "8", processDepth: "8", encoder: "av1_nvenc", decode: "d3d11"
        },
        "artifact": {
            tool: "vfx",
            chain: [{ effect: "artifact", quality: "conservative", scale: "2", strength: "" }],
            inputDepth: "8", processDepth: "8", encoder: "av1_nvenc", decode: "d3d11"
        },
        "chain": {
            tool: "vfx",
            chain: [
                { effect: "artifact", quality: "conservative", scale: "2", strength: "" },
                { effect: "superres", quality: "ultra", scale: "2", strength: "" }
            ],
            inputDepth: "8", processDepth: "8", encoder: "av1_nvenc", decode: "d3d11"
        },
        "hdr": {
            tool: "vfx",
            chain: [{ effect: "hdr", quality: "high", scale: "2", strength: "" }],
            inputDepth: "8", processDepth: "10", encoder: "hevc_nvenc", decode: "d3d11"
        },
        "fastupscale": {
            tool: "upscale", upscaleRatio: "2", upscaleStrength: "0.4",
            encoder: "av1_nvenc", decode: "d3d11"
        },
        "trtdenoise": {
            tool: "denoise", denoiseStrength: "0.5",
            encoder: "av1_nvenc", decode: "d3d11"
        },
        "rate": {
            tool: "framerate", framerateMultiplier: "2", framerateMode: "medium",
            framerateEncoding: "rgb8", inputDepth: "8",
            encoder: "av1_nvenc", decode: "d3d11"
        },
        "framegen": {
            tool: "framegen", framegenMode: "medium", framegenMethod: "time",
            framegenTimestep: "0.50", framegenEncoding: "rgb8", inputDepth: "8",
            encoder: "av1_nvenc", decode: "d3d11"
        }
    };

    var rcState = { vbr: "6000k", cbr: "6000k", cq: "30", constqp: "20" };
    var lastRc = "vbr";

    function applyRcField(rc) {
        var input = $("rcValue");
        var label = $("rcValueLabel");
        var help = $("rcHelp");
        input.removeAttribute("min");
        input.removeAttribute("max");
        input.removeAttribute("step");
        if (rc === "vbr" || rc === "cbr") {
            label.textContent = "Bitrate";
            input.type = "text";
            input.value = rcState[rc] || "6000k";
            input.placeholder = "6000k";
            help.textContent = "VBR and CBR use a target bitrate.";
        } else if (rc === "cq") {
            label.textContent = "CQ value";
            input.type = "number";
            input.min = "0";
            input.max = "51";
            input.step = "1";
            input.value = rcState[rc] || "30";
            input.placeholder = "30";
            help.textContent = "CQ: lower value is better quality (0-51).";
        } else {
            label.textContent = "QP value";
            input.type = "number";
            input.min = "0";
            input.max = "51";
            input.step = "1";
            input.value = rcState[rc] || "20";
            input.placeholder = "20";
            help.textContent = "Constant QP: lower value is better quality (0-51).";
        }
    }

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
            tool: value("tool"),
            input: value("input"),
            inputB: value("framegenInputB"),
            output: value("output"),
            libDir: value("libDir"),
            shell: value("shell"),
            chain: readChain(),
            inputDepth: value("inputDepth"),
            processDepth: value("processDepth"),
            hdrVsr: checked("hdrVsr"),
            outW: value("outW"),
            outH: value("outH"),
            upscaleRatio: value("upscaleRatio"),
            upscaleStrength: value("upscaleStrength"),
            denoiseStrength: value("denoiseStrength"),
            denoiseModelDir: value("denoiseModelDir"),
            framerateMultiplier: value("framerateMultiplier"),
            framerateMode: value("framerateMode"),
            framerateEncoding: value("framerateEncoding"),
            framerateAutoShot: checked("framerateAutoShot"),
            framegenMode: value("framegenMode"),
            framegenMethod: value("framegenMethod"),
            framegenTimestep: value("framegenTimestep"),
            framegenMultiplier: value("framegenMultiplier"),
            framegenFrameIndex: value("framegenFrameIndex"),
            framegenEncoding: value("framegenEncoding"),
            framegenAutoShot: checked("framegenAutoShot"),
            decode: value("decode"),
            encoder: value("encoder"),
            preset: value("preset"),
            tune: value("tune"),
            rc: value("rc"),
            bitrate: (value("rc") === "vbr" || value("rc") === "cbr") ? value("rcValue") : rcState.vbr,
            cq: value("rc") === "cq" ? value("rcValue") : rcState.cq,
            qp: value("rc") === "constqp" ? value("rcValue") : rcState.qp,
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

    function show(name, on) {
        document.querySelectorAll("[data-show='" + name + "']").forEach(function (el) {
            el.classList.toggle("is-hidden", !on);
        });
    }

    function update() {
        var rc = value("rc");
        if (rc !== lastRc) {
            rcState[lastRc] = value("rcValue");
            lastRc = rc;
            applyRcField(rc);
        }

        var tool = value("tool");
        var chain = readChain();
        var hasHdrEffect = chain.some(function (e) { return e.effect === "hdr"; });
        var hasArtifact = chain.some(function (e) { return e.effect === "artifact"; });
        var hasDenoise = chain.some(function (e) { return e.effect === "denoise"; });
        var vsrStages = chain.filter(function (e) {
            return G.vsrEffects.indexOf(e.effect) >= 0;
        });
        var superresStages = chain.filter(function (e) { return e.effect === "superres"; });
        var inputDepth = value("inputDepth");
        var processDepth = value("processDepth");
        var frTen = tool === "framerate" && value("framerateEncoding") === "rgb10a2";
        var fgTen = tool === "framegen" && value("framegenEncoding") === "rgb10a2";

        $("vfxSection").classList.toggle("is-hidden", tool !== "vfx");
        $("upscaleSection").classList.toggle("is-hidden", tool !== "upscale");
        $("denoiseSection").classList.toggle("is-hidden", tool !== "denoise");
        $("framerateSection").classList.toggle("is-hidden", tool !== "framerate");
        $("framegenSection").classList.toggle("is-hidden", tool !== "framegen");
        $("inputLabel").textContent = tool === "framegen" ? "Previous input" : "Input";
        show("indepth", tool === "vfx" || tool === "framerate" || tool === "framegen");
        show("fgtime", tool === "framegen" && value("framegenMethod") === "time");
        show("fgindex", tool === "framegen" && value("framegenMethod") === "index");

        var hdrBox = $("hdrVsr");
        var hdrAllowed = tool === "vfx" && processDepth === "10" &&
                         inputDepth === "10" && superresStages.length > 0 &&
                         !hasArtifact && !hasHdrEffect;
        if (!hdrAllowed)
            hdrBox.checked = false;
        show("hdrvsr", hdrAllowed);
        var hdrVsr = hdrAllowed && hdrBox.checked;

        show("depth", tool === "vfx" && vsrStages.length > 0);
        show("outdims", tool === "vfx" && superresStages.length > 0);

        var tenBit = tool === "vfx" && processDepth === "10" &&
                     vsrStages.length > 0 && !hasArtifact;
        var hasHDR = tool === "vfx" && (hasHdrEffect || hdrVsr);
        var force10 = hasHDR || tenBit || frTen || fgTen;

        var enc = $("encoder");
        var h264 = enc.querySelector("option[value='h264_nvenc']");
        h264.disabled = force10;
        if (force10 && enc.value === "h264_nvenc")
            enc.value = "hevc_nvenc";

        var notes = [];
        if (tool === "vfx") {
            if (hdrVsr)
                notes.push("VSR preserves PQ/BT.2020; output is X2BGR10 with HDR10 tags. Convert with swscale before encoding.");
            if (hasHdrEffect)
                notes.push("HDR output is X2BGR10; the chain converts it to P010 HDR10. Use HEVC or AV1.");
            if (tenBit && !hasArtifact) {
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
            if (value("outW") || value("outH"))
                notes.push("Output W/H apply to the last superres stage; downscaling is rejected.");
        } else if (tool === "upscale") {
            notes.push("Fast upscale is 8-bit only. Ratios 4/3, 3/2, 2, 3, 4; 2.5x and 1x are rejected.");
        } else if (tool === "denoise") {
            notes.push("TRT denoise is 8-bit only; P010 is downconverted. It needs the SDK 1.3 Denoise models (models folder under the runtime libs).");
        } else if (tool === "framegen") {
            notes.push("Frame generation takes two independent, same-size inputs and writes one intermediate frame per pair. Align their timestamps before pairing.");
            if (fgTen)
                notes.push("RGB10A2 output is P010; 8-bit input is promoted on both paths.");
        } else {
            notes.push("Frame-rate conversion emits multiplier × frames. RGB10A2 needs 10-bit input; 8-bit is promoted to P010.");
        }
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
        if (p.chain)
            renderChain(p.chain);
        ["tool", "inputDepth", "processDepth", "encoder", "decode",
         "upscaleRatio", "upscaleStrength", "denoiseStrength",
         "framerateMultiplier", "framerateMode", "framerateEncoding",
         "framegenMode", "framegenMethod", "framegenTimestep",
         "framegenMultiplier", "framegenFrameIndex", "framegenEncoding"].forEach(function (id) {
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

    applyRcField("vbr");
    renderChain(G.defaults.chain);
    update();
}());
