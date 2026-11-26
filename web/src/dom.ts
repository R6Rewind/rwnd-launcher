export type Child = Node|string|null|undefined|false|Child[];
type Attrs = Record<string, any>;

export function h(spec: string, attrs?: Attrs|Child, ...children: Child[]): HTMLElement {
    const [tag, ...classes] = spec.split('.');
    const el = document.createElement(tag || 'div');
    if (classes.length) el.className = classes.join(' ');
    if (attrs !== undefined &&
        (attrs === null || typeof attrs !== 'object' || attrs instanceof Node || Array.isArray(attrs))) {
        children.unshift(attrs as Child);
        attrs = undefined;
    }
    for (const [key, value] of Object.entries((attrs ?? {}) as Attrs)) {
        if (value === undefined || value === null || value === false) continue;
        if (key.startsWith('on') && typeof value === 'function')
            el.addEventListener(key.slice(2).toLowerCase(), value);
        else if (key === 'style' && typeof value === 'object')
            Object.assign(el.style, value);
        else if (key === 'class')
            el.className += (el.className ? ' ' : '') + value;
        else if (key === 'html')
            el.innerHTML = value;
        else if (key === 'text')
            el.textContent = value;
        else if (key in el && typeof value !== 'string')
            (el as any)[key] = value;
        else
            el.setAttribute(key, value === true ? '' : String(value));
    }
    append(el, children);
    return el;
}

export function append(el: Element, children: Child[]) {
    for (const child of (children as any[]).flat(Infinity) as Child[]) {
        if (child === null || child === undefined || child === false) continue;
        el.append(typeof child === 'string' ? document.createTextNode(child) : child as Node);
    }
}

export function svg(markup: string): Element {
    const t = document.createElement('template');
    t.innerHTML = markup.trim();
    return t.content.firstElementChild!;
}

export const show = (el: Element, visible: boolean) => { el.classList.toggle('hidden', !visible);};
export const icon = (glyph: string, cls = '') => h('span.icon' + (cls ? '.' + cls : ''), glyph);

export const tracked = (text: string) => [...text].map(c => c === ' ' ? ' ' : c).join(' ');

export const sleep = (ms: number) => new Promise<void>(resolve => setTimeout(resolve, ms));

export const $ = <T extends HTMLElement = HTMLElement>(selector: string, root: ParentNode = document) =>
    root.querySelector(selector) as T;

export function focusVisual(el: HTMLElement) {
    for (const other of document.querySelectorAll('.focused')) other.classList.remove('focused');
    el.focus();
    el.classList.add('focused');
}
document.addEventListener('focusin', e => {
    for (const other of document.querySelectorAll('.focused'))
        if (other !== e.target) other.classList.remove('focused');
});
document.addEventListener('mousedown', () => {
    for (const other of document.querySelectorAll('.focused')) other.classList.remove('focused');
});
