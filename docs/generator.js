(function (root, factory) {
    if (typeof module === "object" && module.exports) {
        module.exports = factory();
    } else {
        root.NVVFXGen = factory();
    }
}(typeof self !== "undefined" ? self : this, function () {
    "use strict";

    var MODES = ["superres", "denoise", "deblur", "artifact",
                 "artifact+superres", "hdr"];

    var defaults = {
        binary: "ffmpeg-nvvfx",
        input: "input.mkv",
        output: "output.mkv",
        libDir: "",
        shell: "bash",
        mode: "superres",
        quality: "ultra",
        artifactQuality: "conservative",
        scale: "2",
        bitDepth: "8",
        strength: "",
        decode: "d3d11",
        encoder: "av1_nvenc",
        preset: "p5",
        bitrate: "6000k",
        duration: "",
        copyAudio: true,
        copySubs: true,
        copyAttachments: true,
        overwrite: true
    };

    function isVSR(mode) {
        return mode === "superres" || mode === "denoise" || mode === "deblur";
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

    function buildCommand(options) {
        var s = Object.assign({}, defaults, options || {});
        var mode = MODES.indexOf(s.mode) >= 0 ? s.mode : "superres";
        var vsr = isVSR(mode);
        var chain = mode === "artifact+superres";
        var hdr = mode === "hdr";
        var artifact = mode === "artifact" || chain;
        var tenBit = vsr && String(s.bitDepth) === "10";
        var encoder = s.encoder || "av1_nvenc";

        /* H.264 NVENC has no 10-bit mode. */
        if ((hdr || tenBit) && encoder === "h264_nvenc")
            encoder = "hevc_nvenc";

        var filters = [];
        if (s.decode === "nvdec")
            filters.push((tenBit ? "format=p010,hwupload" : "format=nv12,hwupload"));

        if (vsr) {
            var vf = "nvvfx=mode=" + mode + ":quality=" + s.quality;
            if (mode === "superres")
                vf += ":scale=" + s.scale;
            if (tenBit)
                vf += ":bit_depth=10";
            if (s.strength !== "" && s.strength != null)
                vf += ":strength=" + s.strength;
            filters.push(vf);
        } else if (mode === "artifact") {
            filters.push("nvvfx=mode=artifact:quality=" + s.artifactQuality);
        } else if (chain) {
            filters.push("nvvfx=mode=artifact:quality=" + s.artifactQuality);
            var cf = "nvvfx=mode=superres:quality=" + s.quality + ":scale=" + s.scale;
            if (s.strength !== "" && s.strength != null)
                cf += ":strength=" + s.strength;
            filters.push(cf);
        } else if (hdr) {
            filters.push("nvvfx=mode=hdr");
        }

        if (hdr) {
            filters.push("hwdownload,format=x2bgr10le," +
                         "scale=in_range=full:out_color_matrix=bt2020nc:out_range=tv," +
                         "format=p010le");
        } else if (!tenBit) {
            filters.push("scale_d3d11=format=nv12");
        }

        var vfString = filters.join(",");
        var parts = [];

        if (s.libDir) {
            if (s.shell === "cmd")
                parts.push("set \"NVVFX_LIB_DIR=" + s.libDir + "\" &&");
            else
                parts.push("NVVFX_LIB_DIR='" + String(s.libDir).replace(/'/g, "'\\''") + "'");
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

        if (hdr) {
            if (encoder === "hevc_nvenc")
                parts.push("-profile:v", "main10");
            parts.push("-pix_fmt", "p010le");
        } else if (tenBit && encoder === "hevc_nvenc") {
            parts.push("-profile:v", "main10");
        }

        parts.push("-preset", s.preset || "p5");
        parts.push("-tune", "hq");
        parts.push("-rc", "vbr");
        parts.push("-b:v", s.bitrate || "6000k");
        parts.push("-maxrate", s.bitrate || "6000k");
        parts.push("-bufsize", bitrateKbps(s.bitrate) * 2 + "k");
        parts.push("-multipass", "qres");

        if (hdr) {
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
        modes: MODES,
        bitrateKbps: bitrateKbps,
        buildCommand: buildCommand
    };
}));
