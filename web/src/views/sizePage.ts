import {on, rpc, isCanceled, messageOf} from '../bridge';
import {h, icon, show, svg, tracked} from '../dom';
import {rBlock} from '../brand';
import {AmbientBackdrop, MediaBackdrop} from '../backdrop';
import {Progress} from '../controls';
import {shell, type Snapshot, type View} from '../shell';
import {confirmDialog} from './dialogs';
import {outbreakDialog, type SteamSignIn} from './outbreak';

export type DownloadSize = 'full'|'medium'|'small';
export const SIZES: DownloadSize[] = [ 'full', 'medium', 'small' ];
export const SIZE_INFO: Record < DownloadSize, {
    name: string;
    line: string
}
> = {
    full : {name : 'Full', line : 'Best look'},
    medium : {name : 'Medium', line : 'Sharp'},
    small : {name : 'Small', line : 'Low textures'},
};
let sizeBytes: Record<DownloadSize, number> = {full : 68_516_085_340, medium : 36_710_350_428, small : 29_155_229_276};
export const gib = (bytes: number) => Math.round(bytes / 1024 ** 3);
export const sizeGb = (size: DownloadSize) => gib(sizeBytes[size]);
export async function loadSizes() {
    const answer = await rpc<Record<DownloadSize, number>>('gameFiles.sizes').catch(() => null);
    if (answer?.full && answer.medium && answer.small) sizeBytes = answer;
}
export const parseSize = (value: string|undefined): DownloadSize =>
    value === 'medium' || value === 'small' ? value : 'full';

export interface SizePage {
    el: HTMLElement;
    back: HTMLButtonElement;
    get value(): DownloadSize;
    set value(v: DownloadSize);
    setCurrent(size: DownloadSize|null): void;
    dispose(): void;
}

export function sizePage(eyebrow: string, actions: HTMLElement, onChange: (size: DownloadSize) => void,
                         embedded = false): SizePage {
    const media = new MediaBackdrop();
    const ambient = new AmbientBackdrop(media);
    const back = h('button.link.no-drag', {type : 'button'}, icon(''), h('span', 'Back')) as HTMLButtonElement;
    const mark = h('div.mark');
    mark.append(svg(rBlock()));
    let value: DownloadSize = 'medium';
    const cards = {} as Record < DownloadSize, {
        el: HTMLButtonElement;
        chip: HTMLElement;
        gbText: HTMLElement
    }
    > ;
    for (const size of SIZES) {
        const info = SIZE_INFO[size];
        const chip = h('div.chip.size-chip.hidden', h('span.eyebrow', ''));
        const gbText = h('div.size-gb');
        const el = h('button.size-card.glass', {type : 'button'},
                     h('div.size-head', h('div.title.size-name', info.name), chip), gbText,
                     h('div.body.size-line', info.line)) as HTMLButtonElement;
        el.onclick = () => {
            if (value !== size) {
                value = size;
                render();
                onChange(size);
            }
        };
        cards[size] = {el, chip, gbText};
    }
    let current: DownloadSize|null = null;
    const render = () => {
        for (const size of SIZES) {
            const c = cards[size];
            c.el.classList.toggle('checked', size === value);
            c.gbText.textContent = sizeGb(size) + ' GB';
            const label = size === current ? 'CURRENT' : size === 'medium' && !current ? 'RECOMMENDED' : '';
            (c.chip.firstElementChild as HTMLElement).textContent = tracked(label);
            show(c.chip, label !== '');
        }
    };
    render();
    loadSizes().then(render);
    const main = h('div.size-main', h('div.eyebrow.accent', tracked(eyebrow)),
                   h('div.title', {style : {fontSize : '34px', margin : '6px 0 22px'}}, 'Download size'),
                   h('div.size-cards', ...SIZES.map(s => cards[s].el)), actions);
    const el = embedded ? main
                        : h('div', {style : {position : 'absolute', inset : '0'}}, ambient.el,
                            h('div.size-page', h('div.size-top', mark, back), main));
    return {
        el,
        back,
        get value() { return value; },
        set value(v: DownloadSize) {
            value = v;
            render();
        },
        setCurrent(size) {
            current = size;
            render();
        },
        dispose() {
            media.dispose();
            ambient.dispose();
        },
    };
}

export interface SizeView extends View {
    ready: Promise<void>;
    close(): void;
    previewPick(size: DownloadSize): void;
    previewConfirm(size: DownloadSize): void;
    previewSteam(size: DownloadSize): void;
    previewProgress(size: DownloadSize): void;
}

export function sizeView(options: {embedded?: boolean; onClose?: () => void} = {}): SizeView {
    const embedded = !!options.embedded;
    const switchButton = h('button.btn.primary.big', {type : 'button', style : {width : '260px'}, disabled : true},
                           h('span', 'Switch')) as HTMLButtonElement;
    const note = h('div.body', {style : {marginLeft : '18px', fontSize : '15px'}});
    const row = h('div', {style : {display : 'flex', alignItems : 'center', marginTop : '26px'}}, switchButton, note);
    const stage = h('div.strong', {style : {fontSize : '15px', flex : '1'}});
    const bar = new Progress();
    bar.el.style.marginTop = '8px';
    const cancel = h('button.link', {type : 'button', style : {fontSize : '14px', marginLeft : '12px'}}, 'Cancel') as
                   HTMLButtonElement;
    const detail = h('div.body.nowrap',
                     {style : {fontSize : '13px', color : 'var(--dim)', marginTop : '8px', whiteSpace : 'nowrap'}});
    const progress = h('div.hidden', {style : {marginTop : '26px', width : '520px'}},
                       h('div', {style : {display : 'flex'}}, stage, cancel), bar.el, detail);
    const actions = h('div', row, progress);
    let current: DownloadSize = 'full';
    const page = sizePage('GAME FILES', actions, () => describe(), embedded);
    const leave = () => {
        if (embedded)
            options.onClose?.();
        else
            shell.showHome();
    };
    const confirm = confirmDialog({eyebrow : 'GAME FILES', glyph : '', title : '', text : '', ok : 'Switch'});
    let call: {cancel(): void}|null = null;
    const steam = outbreakDialog(choice => download(choice), () => call?.cancel(),
                                 {eyebrow : 'GAME FILES', glyph : '', title : '', text : ''});
    const closeX =
        h('button.chrome-button.ob-close', {type : 'button', title : 'Close'}, '\uE8BB') as HTMLButtonElement;
    const el =
        embedded ? h('div.ob-modal', h('div.ob-dialog.glass-strong.size-dialog', closeX, page.el), confirm.el, steam.el)
                 : h('div', {style : {position : 'absolute', inset : '0'}}, page.el, confirm.el, steam.el);
    closeX.onclick = () => {
        if (!call) leave();
    };
    if (embedded)
        el.addEventListener('mousedown', e => {
            if (e.target === el && !call) leave();
        });
    const down = (to: DownloadSize) => SIZES.indexOf(to) > SIZES.indexOf(current);
    const delta = (to: DownloadSize) => Math.abs(sizeGb(current) - sizeGb(to));

    function describe() {
        const to = page.value;
        switchButton.disabled = to === current || call !== null;
        note.textContent = to === current || down(to) ? '' : `${delta(to)} GB from Steam.`;
    }
    const steamTexts = (to: DownloadSize, bytes?: number) => ({
        title : 'Download ' + SIZE_INFO[to].name,
        text : `${bytes != null ? Math.max(1, gib(bytes)) : delta(to)} GB from Steam.`
    });
    const gbText = (bytes: number) => bytes >= 1024 ** 3 ? `${gib(bytes)} GB` : `${(bytes / 1024 ** 3).toFixed(1)} GB`;
    const confirmText = (to: DownloadSize, bytes: number) => ({
        title : `Switch to ${SIZE_INFO[to].name}?`,
        text : `${SIZE_INFO[to].line}. ` + (bytes > 0 ? `Frees ${gbText(bytes)}.` : 'Your game folder keeps its files.')
    });
    const switchedText = (to: DownloadSize, freed?: number) =>
        `Switched to ${SIZE_INFO[to].name}.` + (freed && freed > 0 ? ` Freed ${gbText(freed)}.` : '');
    async function switchTo() {
        const to = page.value;
        if (to === current || call || shell.busy) return;
        shell.busy = true;
        try {
            if (down(to)) {
                const plan = await rpc<{result : string; bytes?: number}>('gameFiles.setSize', {size : to});
                shell.busy = false;
                if (!(await confirm.ask(confirmText(to, plan.bytes ?? 0)))) return;
                shell.busy = true;
                const done =
                    await rpc<{result : string; freed?: number}>('gameFiles.setSize', {size : to, confirm : true});
                shell.busy = false;
                leave();
                shell.showToast(switchedText(to, done.freed));
                return;
            }
            const result = await rpc<{result : string; bytes?: number}>('gameFiles.setSize', {size : to});
            shell.busy = false;
            if (result.result === 'steam') {
                steam.open(steamTexts(to, result.bytes));
                return;
            }
            leave();
            shell.showToast(switchedText(to));
        } catch (e) {
            shell.busy = false;
            shell.showToast(messageOf(e));
        }
    }
    async function download(choice: SteamSignIn) {
        const to = page.value;
        if (call) return;
        shell.busy = true;
        show(row, false);
        show(progress, true);
        show(page.back, false);
        stage.textContent = 'Starting...';
        detail.textContent = '';
        bar.indeterminate = true;
        const off = on('steam.progress', p => {
            stage.textContent =
                'Downloading ' + SIZE_INFO[to].name + (p.fraction != null ? ` · ${Math.floor(p.fraction * 100)}%` : '');
            bar.indeterminate = p.fraction == null;
            if (p.fraction != null) bar.value = p.fraction * 100;
            detail.textContent = p.detail ?? '';
            if (steam.isOpen && /^Downloading/.test(p.stage)) steam.started();
        });
        const offAsk =
            [ on('steam.prompt', p => steam.ask(p.kind, p.email, p.retry)), on('steam.qr', q => steam.qr(q.rows)) ];
        const c = rpc('gameFiles.setSize', {size : to, steam : true, ...choice});
        call = c;
        try {
            await c;
            steam.close();
            shell.busy = false;
            leave();
            shell.showToast(`Switched to ${SIZE_INFO[to].name}.`);
            return;
        } catch (e) {
            if (isCanceled(e)) {
                steam.close();
                shell.showToast('Paused. Switch again to continue.');
            } else if (steam.isOpen)
                steam.error(messageOf(e));
            else
                shell.showToast(messageOf(e));
        } finally {
            off();
            offAsk.forEach(o => o());
            call = null;
            shell.busy = false;
        }
        show(row, true);
        show(progress, false);
        show(page.back, true);
        describe();
    }
    switchButton.onclick = () => switchTo();
    cancel.onclick = () => call?.cancel();
    page.back.onclick = () => {
        if (!call) leave();
    };

    const ready = (async () => {
        const snapshot = await rpc<Snapshot>('session.snapshot');
        current = parseSize(snapshot.prefs.downloadSize);
        page.setCurrent(current);
        page.value = current;
        await loadSizes();
        describe();
    })();
    return {
        el,
        ready,
        dispose() {
            call?.cancel();
            page.dispose();
        },
        close() {
            if (!call) leave();
        },
        previewPick(size) {
            page.value = size;
            describe();
        },
        previewConfirm(size) {
            page.value = size;
            describe();
            void confirm.ask(confirmText(size, delta(size) * 1024 ** 3));
        },
        previewSteam(size) {
            page.value = size;
            describe();
            steam.open(steamTexts(size));
        },
        previewProgress(size) {
            page.value = size;
            describe();
            show(row, false);
            show(progress, true);
            show(page.back, false);
            stage.textContent = `Downloading ${SIZE_INFO[size].name} · 37%`;
            detail.textContent = 'datapc64_merged_bnk_000000006_textures3.forge  ·  12.4 GB downloaded';
            bar.indeterminate = false;
            bar.value = 37;
        },
    };
}
