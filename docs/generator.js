(function (root, factory) {
    if (typeof module === "object" && module.exports) {
        module.exports = factory();
    } else {
        root.NVVFXGen = factory();
    }
}(typeof self !== "undefined" ? self : this, function () {
    "use strict";

    var TOOLS = ["vfx", "upscale", "denoise", "framerate"];
    var EFFECTS = ["superres", "denoise", "deblur", "artifact", "hdr"];
    var VSR_EFFECTS = ["superres", "denoise", "deblur"];
    var VSR_QUALITY = ["low", "medium", "high", "ultra"];
    var ARTIFACT_QUALITY = ["conservative", "aggressive"];
    var RATE_CONTROL = ["vbr", "cq", "constqp", "cbr"];
    var MULTIPASS = ["disabled", "qres", "fullres"];
    var TUNES = ["hq", "ll", "ull"];
    var UPSCALE_RATIOS = ["4/3", "3/2", "2", "3", "4"];

    var defaults = {
        binary: "ffmpeg-nvvfx",
        tool: "vfx",
        input: "input.mkv",
        output: "output.mkv",
        libDir: "",
        shell: "bash",
        chain: [{ effect: "superres", quality: "ultra", scale: "2", strength: "" }],
        inputDepth: "8",
        processDepth: "8",
        hdrVsr: false,
        outW: "",
        outH: "",
        upscaleRatio: "2",
        upscaleStrength: "0.4",
        denoiseStrength: "0.5",
        denoiseModelDir: "",
        framerateMultiplier: "2",
        framerateMode: "medium",
        framerateEncoding: "rgb8",
        framerateAutoShot: true,
        decode: "d3d11",
        encoder: "av1_nvenc",
        preset: "p5",
        tune: "hq",
        rc: "vbr",
        bitrate: "6000k",
        cq: "30",
        qp: "20",
        multipass: "qres",
        lookahead: "32",
        spatialAq: true,
        temporalAq: true,
        bf: "4",
        gop: "240",
        duration: "",
        copyAudio: true,
        copySubs: true,
        copyAttachments: true,
        overwrite: true
    };

    function isVSR(effect) { return VSR_EFFECTS.indexOf(effect) >= 0; }
    function isHDR(effect) { return effect === "hdr"; }
    function isArtifact(effect) { return effect === "artifact"; }

    function clampInt(value, min, max, fallback) {
        var n = parseInt(String(value), 10);
        if (!isFinite(n))
            n = fallback;
        return Math.min(max, Math.max(min, n));
    }

    function bitrateKbps(value) {
        var m = String(value || "6000").match(/([0-9.]+)\s*([kKmM]?)/);
        if (!m)
            return 6000;
        var n = parseFloat(m[1]);
        if (m[2].toLowerCase() === "m")
            n *= 1000;
        return Math.max(1, Math.round(n));
    }

    function quotePath(value) {
        return '"' + String(value == null ? "" : value).replace(/"/g, '\\"') + '"';
    }

    function cookieCut(value) {
        return String(value == null ? "" : value).replace(/'/g, "'\\''");
    }

    function filterPath(value) {
        return String(value == null ? "" : value)
            .replace(/\\/g, "/")
            .replace(/:/g, "\\:");
    }

    function normalizeChain(s) {
        var chain = Array.isArray(s.chain) ? s.chain.slice() : [];

        if (!chain.length && s.mode) {
            if (s.mode === "artifact+superres") {
                chain = [
                    { effect: "artifact", quality: s.artifactQuality || "conservative" },
                    { effect: "superres", quality: s.quality || "high",
                      scale: s.scale || "2", strength: s.strength || "" }
                ];
            } else {
                chain = [{
                    effect: s.mode,
                    quality: s.mode === "artifact"
                        ? (s.artifactQuality || "conservative")
                        : (s.quality || "high"),
                    scale: s.scale || "2",
                    strength: s.strength || ""
                }];
            }
        }

        chain = chain.filter(function (e) {
            return e && EFFECTS.indexOf(e.effect) >= 0;
        });

        if (!chain.length)
            chain = [{ effect: "superres", quality: "ultra", scale: "2", strength: "" }];

        var hdr = null;
        chain = chain.filter(function (e) {
            if (e.effect === "hdr") {
                hdr = e;
                return false;
            }
            return true;
        });
        if (hdr)
            chain.push(hdr);

        return chain.map(function (e) {
            var entry = {
                effect: e.effect,
                quality: e.quality,
                scale: e.scale || "2",
                strength: e.strength == null ? "" : String(e.strength)
            };
            if (isArtifact(entry.effect)) {
                if (ARTIFACT_QUALITY.indexOf(entry.quality) < 0)
                    entry.quality = "conservative";
            } else if (isVSR(entry.effect)) {
                if (VSR_QUALITY.indexOf(entry.quality) < 0)
                    entry.quality = "high";
                if (["2", "3", "4"].indexOf(String(entry.scale)) < 0)
                    entry.scale = "2";
            } else {
                entry.quality = "high";
            }
            return entry;
        });
    }

    function buildCommand(options) {
        var legacyDepth = options && options.bitDepth;
        var s = Object.assign({}, defaults, options || {});
        if (legacyDepth && (!options.inputDepth && !options.processDepth)) {
            s.inputDepth = String(legacyDepth);
            s.processDepth = String(legacyDepth);
        }

        var tool = TOOLS.indexOf(s.tool) >= 0 ? s.tool : "vfx";
        var inputDepth = String(s.inputDepth) === "10" ? "10" : "8";
        var processDepth = String(s.processDepth) === "10" ? "10" : "8";
        var chain = normalizeChain(s);
        var hasArtifact = chain.some(function (e) { return isArtifact(e.effect); });
        var hasHdrEffect = chain.some(function (e) { return isHDR(e.effect); });
        var vsrStages = chain.filter(function (e) { return isVSR(e.effect); });
        var superresStages = chain.filter(function (e) { return e.effect === "superres"; });
        var tenBit = tool === "vfx" && processDepth === "10" &&
                     vsrStages.length > 0 && !hasArtifact;
        var hdrVsr = tool === "vfx" && s.hdrVsr && tenBit &&
                     !hasHdrEffect && superresStages.length > 0;
        var hasHDR = tool === "vfx" && (hasHdrEffect || hdrVsr);
        var promote = tenBit && inputDepth === "8";
        var frTen = tool === "framerate" && s.framerateEncoding === "rgb10a2";
        var force10 = hasHDR || tenBit || frTen;

        var rc = RATE_CONTROL.indexOf(s.rc) >= 0 ? s.rc : "vbr";
        var multipass = MULTIPASS.indexOf(s.multipass) >= 0 ? s.multipass : "qres";
        var tune = TUNES.indexOf(s.tune) >= 0 ? s.tune : "hq";
        var cq = clampInt(s.cq, 0, 51, 30);
        var qp = clampInt(s.qp, 0, 51, 20);
        var lookahead = clampInt(s.lookahead, 0, 64, 32);
        var bf = clampInt(s.bf, 0, 4, 4);
        var gop = clampInt(s.gop, 0, 2000, 240);
        var kbps = bitrateKbps(s.bitrate);
        var encoder = s.encoder || "av1_nvenc";

        if (force10 && encoder === "h264_nvenc")
            encoder = "hevc_nvenc";

        var filters = [];

        if (s.decode === "nvdec") {
            if (tool === "vfx") {
                filters.push(tenBit && inputDepth === "10" ? "format=p010,hwupload"
                                                           : "format=nv12,hwupload");
                if (promote)
                    filters.push("scale_d3d11=format=p010");
            } else if (tool === "framerate") {
                filters.push(frTen && inputDepth === "10" ? "format=p010,hwupload"
                                                          : "format=nv12,hwupload");
                if (frTen && inputDepth === "8")
                    filters.push("scale_d3d11=format=p010");
            } else {
                filters.push("format=nv12,hwupload");
            }
        } else {
            if (tool === "vfx" && promote)
                filters.push("scale_d3d11=format=p010");
            if (tool === "framerate" && frTen && inputDepth === "8")
                filters.push("scale_d3d11=format=p010");
        }

        if (tool === "vfx") {
            var outW = parseInt(s.outW, 10);
            var outH = parseInt(s.outH, 10);
            var useOut = outW > 0 && outH > 0 && superresStages.length > 0;

            chain.forEach(function (e) {
                var f = "nvvfx=mode=" + e.effect;
                if (isVSR(e.effect)) {
                    f += ":quality=" + e.quality;
                    if (e.effect === "superres")
                        f += ":scale=" + e.scale;
                    if (tenBit)
                        f += ":bit_depth=10";
                    if (hdrVsr && e.effect === "superres")
                        f += ":hdr=on";
                    if (useOut && e.effect === "superres" &&
                        e === superresStages[superresStages.length - 1])
                        f += ":out_w=" + outW + ":out_h=" + outH;
                    if (e.strength !== "")
                        f += ":strength=" + e.strength;
                } else if (isArtifact(e.effect)) {
                    f += ":quality=" + e.quality;
                }
                filters.push(f);
            });

            if (hasHDR) {
                filters.push("hwdownload,format=x2bgr10le," +
                             "scale=in_range=full:out_color_matrix=bt2020nc:out_range=tv," +
                             "format=p010le");
            } else if (!tenBit) {
                filters.push("scale_d3d11=format=nv12");
            }
        } else if (tool === "upscale") {
            var ratio = UPSCALE_RATIOS.indexOf(String(s.upscaleRatio)) >= 0
                        ? String(s.upscaleRatio) : "2";
            filters.push("nvvfx_upscale=ratio=" + ratio +
                         ":strength=" + (s.upscaleStrength || "0.4"));
            filters.push("scale_d3d11=format=nv12");
        } else if (tool === "denoise") {
            var df = "nvvfx_denoise=strength=" + (s.denoiseStrength || "0.5");
            if (s.denoiseModelDir)
                df += ":model_dir=" + filterPath(s.denoiseModelDir);
            filters.push(df);
            filters.push("scale_d3d11=format=nv12");
        } else {
            var frMode = ["low", "medium", "high"].indexOf(s.framerateMode) >= 0
                         ? s.framerateMode : "medium";
            var frEnc = frTen ? "rgb10a2" : "rgb8";
            var fr = "nvvfx_framerate=multiplier=" +
                     clampInt(s.framerateMultiplier, 2, 8, 2) +
                     ":mode=" + frMode + ":encoding=" + frEnc;
            if (!s.framerateAutoShot)
                fr += ":auto_shot_change=0";
            filters.push(fr);
            if (!frTen)
                filters.push("scale_d3d11=format=nv12");
        }

        var vfString = filters.join(",");
        var parts = [];

        if (s.libDir) {
            if (s.shell === "cmd")
                parts.push("set \"NVVFX_LIB_DIR=" + s.libDir + "\" &&");
            else
                parts.push("NVVFX_LIB_DIR='" + cookieCut(s.libDir) + "'");
        }

        parts.push(s.binary || "ffmpeg-nvvfx");
        if (s.overwrite)
            parts.push("-y");

        if (s.decode === "nvdec") {
            parts.push("-hwaccel", "cuda");
        } else {
            parts.push("-hwaccel", "d3d11va",
                       "-hwaccel_output_format", "d3d11");
        }

        parts.push("-i", quotePath(s.input || "input.mkv"));
        parts.push("-map", "0:v:0");
        if (s.copyAudio)
            parts.push("-map", "0:a?");
        if (s.copySubs)
            parts.push("-map", "0:s?");
        if (s.copyAttachments)
            parts.push("-map", "0:t?");

        parts.push("-vf", '"' + vfString + '"');
        parts.push("-c:v", encoder);

        if (hasHDR) {
            if (encoder === "hevc_nvenc")
                parts.push("-profile:v", "main10");
            parts.push("-pix_fmt", "p010le");
        } else if ((tenBit || frTen) && encoder === "hevc_nvenc") {
            parts.push("-profile:v", "main10");
        }

        parts.push("-preset", s.preset || "p5");
        parts.push("-tune", tune);

        if (rc === "vbr") {
            parts.push("-rc", "vbr",
                       "-b:v", s.bitrate || "6000k",
                       "-maxrate", s.bitrate || "6000k",
                       "-bufsize", kbps * 2 + "k");
        } else if (rc === "cbr") {
            parts.push("-rc", "cbr",
                       "-b:v", s.bitrate || "6000k",
                       "-maxrate", s.bitrate || "6000k",
                       "-bufsize", kbps + "k");
        } else if (rc === "cq") {
            parts.push("-rc", "vbr", "-cq", String(cq), "-b:v", "0");
        } else {
            parts.push("-rc", "constqp", "-qp", String(qp));
        }

        if (multipass !== "disabled")
            parts.push("-multipass", multipass);
        if (lookahead > 0)
            parts.push("-rc-lookahead", String(lookahead));
        if (s.spatialAq)
            parts.push("-spatial-aq", "1");
        if (s.temporalAq)
            parts.push("-temporal-aq", "1");
        if (bf > 0)
            parts.push("-bf", String(bf), "-b_ref_mode", "middle");
        if (gop > 0)
            parts.push("-g", String(gop));

        if (hasHDR) {
            parts.push("-color_range", "tv",
                       "-colorspace", "bt2020nc",
                       "-color_trc", "smpte2084",
                       "-color_primaries", "bt2020");
        }

        if (s.copyAudio)
            parts.push("-c:a", "copy");
        if (s.copySubs)
            parts.push("-c:s", "copy");
        if (s.copyAttachments)
            parts.push("-c:t", "copy");

        if (s.duration)
            parts.push("-t", s.duration);

        parts.push(quotePath(s.output || "output.mkv"));
        return parts.join(" ");
    }

    return {
        defaults: defaults,
        tools: TOOLS,
        effects: EFFECTS,
        vsrEffects: VSR_EFFECTS,
        vsrQuality: VSR_QUALITY,
        artifactQuality: ARTIFACT_QUALITY,
        rateControl: RATE_CONTROL,
        multipass: MULTIPASS,
        tunes: TUNES,
        upscaleRatios: UPSCALE_RATIOS,
        bitrateKbps: bitrateKbps,
        normalizeChain: normalizeChain,
        buildCommand: buildCommand
    };
}));
