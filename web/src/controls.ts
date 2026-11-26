import {h, icon} from './dom';

export function avatar(size: number, name: string, url: string|null|undefined, dot?: string|null): HTMLElement {
    const el = h('div.avatar', {style : {width : size + 'px', height : size + 'px'}});
    el.append(h('div.disc'));
    const first = [...(name ?? '').trim() ][0];
    el.append(h('div.initial', {style : {fontSize : size * 0.42 + 'px'}}, first ? first.toUpperCase() : '?'));
    if (url && url.startsWith('https://')) {
        const img =
            h('img', {src : url, alt : '', draggable : false, referrerpolicy : 'no-referrer'}) as HTMLImageElement;
        img.onerror = () => img.remove();
        el.append(img);
    }
    if (dot) {
        const d = size * 0.32;
        el.append(h('div.dot', {style : {width : d + 'px', height : d + 'px', background : dot}}));
    }
    return el;
}

export class Progress {
    readonly el = h('div.progress.indeterminate', h('div.fill'), h('div.sweep'));
    set indeterminate(value: boolean) { this.el.classList.toggle('indeterminate', value); }
    set value(percent: number) {
        (this.el.firstElementChild as HTMLElement).style.width = Math.max(0, Math.min(100, percent)) + '%';
    }
}

export class CheckBox {
    readonly el: HTMLElement;
    private on: boolean;
    private enabled = true;
    constructor(label: string, checked: boolean, public onChange?: (v: boolean) => void) {
        this.on = checked;
        this.el = h('div.check', {tabindex : 0, role : 'checkbox'}, h('div.box', icon('')), h('span.label', label));
        this.el.addEventListener('click', () => this.toggle());
        this.el.addEventListener('keydown', e => {
            if (e.key === ' ') {
                e.preventDefault();
                this.toggle();
            }
        });
        this.render();
    }
    get checked() { return this.on; }
    set checked(v: boolean) {
        this.on = v;
        this.render();
    }
    set disabled(v: boolean) {
        this.enabled = !v;
        this.el.classList.toggle('disabled', v);
        this.el.tabIndex = v ? -1 : 0;
    }
    private toggle() {
        if (!this.enabled) return;
        this.on = !this.on;
        this.render();
        this.onChange?.(this.on);
    }
    private render() {
        this.el.classList.toggle('checked', this.on);
        this.el.setAttribute('aria-checked', String(this.on));
    }
}

export class Toggle {
    readonly el = h('div.toggle', {tabindex : 0, role: 'switch'}, h('div.track'), h('div.thumb'));
    private on = false;
    private enabled = true;
    constructor(public onChange: (v: boolean) => void) {
        this.el.addEventListener('click', () => this.flip());
        this.el.addEventListener('keydown', e => {
            if (e.key === ' ') {
                e.preventDefault();
                this.flip();
            }
        });
    }
    get checked() { return this.on; }
    set checked(v: boolean) {
        this.on = v;
        this.el.classList.toggle('on', v);
    }
    set disabled(v: boolean) {
        this.enabled = !v;
        this.el.classList.toggle('disabled', v);
    }
    private flip() {
        if (!this.enabled) return;
        this.checked = !this.on;
        this.onChange(this.on);
    }
}

export function textBox(value = '', options: {big?: boolean; password?: boolean; maxLength?: number} = {}) {
    const input =
        h('input', {type : options.password ? 'password' : 'text', spellcheck : false, autocomplete : 'off', value}) as
        HTMLInputElement;
    if (options.maxLength) input.maxLength = options.maxLength;
    const box = h('div.textbox' + (options.big ? '.big' : ''), input);
    box.addEventListener('mousedown', e => {
        if (e.target === box) {
            e.preventDefault();
            input.focus();
        }
    });
    return {box, input};
}

let tipTimer = 0;
export function tooltip(el: HTMLElement, text: string|(() => string | null)) {
    const tip = document.getElementById('tooltip')!;
    el.addEventListener('mouseenter', e => {
        clearTimeout(tipTimer);
        tipTimer = window.setTimeout(() => {
            const value = typeof text === 'function' ? text() : text;
            if (!value) return;
            tip.textContent = value;
            tip.classList.remove('hidden');
            const x = Math.min(e.clientX, window.innerWidth - tip.offsetWidth - 4), y = e.clientY + 20;
            tip.style.left = x + 'px';
            tip.style.top = (y + tip.offsetHeight > window.innerHeight ? e.clientY - tip.offsetHeight - 4 : y) + 'px';
        }, 400);
    });
    el.addEventListener('mouseleave', () => {
        clearTimeout(tipTimer);
        tip.classList.add('hidden');
    });
    el.addEventListener('mousedown', () => {
        clearTimeout(tipTimer);
        tip.classList.add('hidden');
    });
}
