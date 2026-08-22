/*
 * Tape Echo 2 — Chain UI
 *
 * Parameter editor shown when editing the FX slot in chain mode.
 * Jog scrolls the parameter list (4 per page); knobs 1-4 adjust the
 * visible row. Enum values display by name; the footer shows the
 * record-path VU while the echo is running.
 *
 * Tape Echo 2 by Dusk Audio (GPL-3.0); Move port: athousanddetails.
 */

import {
    MoveMainKnob,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4
} from '/data/UserData/schwung/shared/constants.mjs';

import { decodeDelta } from '/data/UserData/schwung/shared/input_filter.mjs';

import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter
} from '/data/UserData/schwung/shared/menu_layout.mjs';

const SCREEN_WIDTH = 128;

/* Mirrors src/dsp/te2_params.h (visible params, same order). */
const PARAMS = [
    { key: "mode", name: "Mode", type: "enum", options: ["H1", "H2", "H3", "H2+3", "H1+R", "H2+R", "H3+R", "H12+R", "H23+R", "H13+R", "H123R", "Rev"] },
    { key: "repeat_rate", name: "Rate", type: "float", min: 0, max: 1, step: 0.02 },
    { key: "intensity", name: "Intensity", type: "float", min: 0, max: 1, step: 0.02 },
    { key: "echo_volume", name: "Echo Vol", type: "float", min: 0, max: 1, step: 0.02 },
    { key: "reverb_volume", name: "Reverb Vol", type: "float", min: 0, max: 1, step: 0.02 },
    { key: "mix", name: "Mix", type: "float", min: 0, max: 1, step: 0.02 },
    { key: "tempo_sync", name: "Tempo Sync", type: "enum", options: ["Off", "On"] },
    { key: "echo_rate_note", name: "Rate Note", type: "int", min: 1, max: 11, step: 1 },
    { key: "input_volume", name: "Drive", type: "float", min: 0, max: 1, step: 0.02 },
    { key: "bass", name: "Bass", type: "float", min: -1, max: 1, step: 0.04 },
    { key: "treble", name: "Treble", type: "float", min: -1, max: 1, step: 0.04 },
    { key: "wow_flutter", name: "Wow/Flutter", type: "float", min: 0, max: 1, step: 0.02 },
    { key: "tape_age", name: "Tape Age", type: "enum", options: ["New", "Used", "Old"] },
    { key: "input_send", name: "Input Send", type: "enum", options: ["Off", "On"] },
    { key: "ping_pong", name: "Ping Pong", type: "enum", options: ["Off", "On"] },
    { key: "stereo_width", name: "Width", type: "int", min: 0, max: 100, step: 5 },
];

const ROWS_PER_PAGE = 4;

let selectedParam = 0;
let paramValues = PARAMS.map(p => p.type === "enum" ? 0 : (p.min || 0));
let noteName = "";
let vuLevel = 0;
let needsRedraw = true;
let tickCount = 0;

function enumIndexOf(param, text) {
    const i = param.options.indexOf(text);
    if (i >= 0) return i;
    const n = parseInt(text, 10);
    return isNaN(n) ? 0 : Math.max(0, Math.min(param.options.length - 1, n));
}

function fetchParams() {
    for (let i = 0; i < PARAMS.length; i++) {
        const p = PARAMS[i];
        const val = host_module_get_param(p.key);
        if (val === null || val === undefined) continue;
        if (p.type === "enum") {
            paramValues[i] = enumIndexOf(p, String(val));
        } else {
            const f = parseFloat(val);
            if (!isNaN(f)) paramValues[i] = f;
        }
    }
    const nn = host_module_get_param("echo_note_name");
    if (nn) noteName = String(nn);
}

function setParam(index, value) {
    const p = PARAMS[index];
    if (p.type === "enum") {
        value = Math.max(0, Math.min(p.options.length - 1, Math.round(value)));
        paramValues[index] = value;
        host_module_set_param(p.key, p.options[value]);
        if (p.key === "preset" || p.key === "mode") {
            /* preset rewrites everything; mode moves the sync note table */
            fetchParams();
        }
    } else {
        value = Math.max(p.min, Math.min(p.max, value));
        paramValues[index] = value;
        host_module_set_param(p.key, p.type === "int"
            ? String(Math.round(value)) : value.toFixed(3));
        if (p.key === "echo_rate_note") {
            const nn = host_module_get_param("echo_note_name");
            if (nn) noteName = String(nn);
        }
    }
}

function adjustParam(index, delta) {
    const p = PARAMS[index];
    const step = p.type === "enum" ? 1 : p.step;
    setParam(index, paramValues[index] + delta * step);
}

function valueText(index) {
    const p = PARAMS[index];
    const v = paramValues[index];
    if (p.type === "enum") return p.options[Math.round(v)] || "?";
    if (p.key === "echo_rate_note")
        return `${Math.round(v)}${noteName ? " " + noteName : ""}`;
    if (p.min < 0) {
        const pct = Math.round(v * 100);
        return (pct > 0 ? "+" : "") + pct + "%";
    }
    return Math.round(v * (p.type === "int" ? 1 : 100)) + (p.type === "int" ? "" : "%");
}

function drawUI() {
    clear_screen();
    drawHeader("Tape Echo 2");

    const page = Math.floor(selectedParam / ROWS_PER_PAGE);
    const listY = 16;
    const lineHeight = 10;

    for (let r = 0; r < ROWS_PER_PAGE; r++) {
        const i = page * ROWS_PER_PAGE + r;
        if (i >= PARAMS.length) break;
        const y = listY + r * lineHeight;
        const isSelected = i === selectedParam;
        if (isSelected) fill_rect(0, y - 1, SCREEN_WIDTH, lineHeight, 1);
        const color = isSelected ? 0 : 1;
        print(2, y, `${isSelected ? ">" : " "}${PARAMS[i].name}`, color);
        const vs = valueText(i);
        print(SCREEN_WIDTH - vs.length * 6 - 4, y, vs, color);
    }

    /* record VU as a footer bar segment count */
    const vu = Math.max(0, Math.min(3, vuLevel));
    const vuStr = "VU" + "|".repeat(Math.round(vu * 2));
    drawFooter({ left: `${page + 1}/${Math.ceil(PARAMS.length / ROWS_PER_PAGE)}`, right: vuStr });
    needsRedraw = false;
}

function init() {
    fetchParams();
    needsRedraw = true;
}

function tick() {
    /* poll the record meter ~4x/sec; redraw only when it visibly moves */
    if ((++tickCount % 11) === 0) {
        const v = parseFloat(host_module_get_param("out_level"));
        if (!isNaN(v) && Math.abs(v - vuLevel) > 0.15) {
            vuLevel = v;
            needsRedraw = true;
        }
    }
    if (needsRedraw) drawUI();
}

function onMidiMessageInternal(data) {
    const status = data[0];
    const d1 = data[1];
    const d2 = data[2];

    if ((status & 0xF0) === 0xB0) {
        if (d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            if (delta !== 0) {
                selectedParam = Math.max(0, Math.min(PARAMS.length - 1, selectedParam + delta));
                needsRedraw = true;
            }
            return;
        }
        if (d1 >= MoveKnob1 && d1 <= MoveKnob4) {
            const knobIndex = d1 - MoveKnob1;
            const page = Math.floor(selectedParam / ROWS_PER_PAGE);
            const target = page * ROWS_PER_PAGE + knobIndex;
            const delta = decodeDelta(d2);
            if (delta !== 0 && target < PARAMS.length) {
                adjustParam(target, delta);
                needsRedraw = true;
            }
            return;
        }
    }
}

globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal
};
