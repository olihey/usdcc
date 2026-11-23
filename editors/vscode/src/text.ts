// Text in and out: what to run, and how its output reads. Nothing of VS Code here.
import { Frame, OutputEvent, RunResult } from './link';

/** Without the lines' common indentation, as the Script Editor's Run Selection has them. */
export function dedent(lines: string[]): string {
    const indents = lines.filter((line) => line.trim()).map((line) => /^[ \t]*/.exec(line)![0]);
    let common = indents[0] ?? '';
    for (const indent of indents) {
        let same = 0;
        while (same < common.length && common[same] === indent[same]) ++same;
        common = common.slice(0, same);
    }
    return lines.map((line) => (line.startsWith(common) ? line.slice(common.length) : line.trimStart())).join('\n') + '\n';
}

/** The lines a selection takes in, from 0: one ending at a line's start leaves that line out. */
export function selectedLines(startLine: number, endLine: number, endCharacter: number): [number, number] {
    return [startLine, endLine > startLine && endCharacter === 0 ? endLine - 1 : endLine];
}

/** A file's path as usdcc writes it: forward slashes, an upper-case drive. */
export function scriptPath(fsPath: string): string {
    return fsPath.replace(/\\/g, '/').replace(/^[a-z]:/, (drive) => drive.toUpperCase());
}

export function outputText(event: OutputEvent): string {
    const plain = event.source === 'python' || event.category === 'plugin';
    return plain ? event.text : `${event.category}: ${event.text}`;
}

export function footer(result: RunResult): string {
    const undo = result.undoSteps === 0 ? '' : result.undoSteps === 1 ? ', 1 undo step' : `, ${result.undoSteps} undo steps`;
    return `    ${result.ok ? 'done in' : 'failed after'} ${result.ms} ms${undo}`;
}

/** `ours`, then what else was there: each once. */
export function mergedPaths(ours: string[], there: string[]): string[] {
    return [...new Set([...ours, ...there])];
}

export interface TracebackLink {
    line: number;
    start: number;
    end: number;
    file: string;
    target: number;
}

/** The places a traceback names, in the lines of some output. */
export function tracebackLinks(text: string): TracebackLink[] {
    const links: TracebackLink[] = [];
    text.split(/\r?\n/).forEach((content, line) => {
        const match = /^(\s*)(File "((?:[A-Za-z]:[\\/]|\/)[^"]*)", line (\d+))/.exec(content);
        if (match) links.push({ line, start: match[1].length, end: match[1].length + match[2].length, file: match[3], target: Number(match[4]) });
    });
    return links;
}

/** The innermost frame in a file that `isOurs`. */
export function innermost(frames: Frame[], isOurs: (file: string) => boolean): Frame | undefined {
    return [...frames].reverse().find((frame) => frame.line > 0 && isOurs(frame.file));
}
