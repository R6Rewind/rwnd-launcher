import {h, icon, svg} from '../dom';
import {rBlock, tagLockup} from '../brand';
import {AmbientBackdrop, MediaBackdrop} from '../backdrop';
import {shell} from '../shell';

export interface SideLayout {
    el: HTMLElement;
    back: HTMLButtonElement;
    dispose(): void;
}

export function sideLayout(content: HTMLElement, options: {scroll?: boolean} = {}): SideLayout {
    const media = new MediaBackdrop();
    const ambient = new AmbientBackdrop(media);
    const back = h('button.link.no-drag', {type : 'button'}, icon(''), h('span', 'Back')) as HTMLButtonElement;
    back.classList.add('hidden');
    const mark = h('div.mark');
    mark.append(svg(rBlock()));
    const lockup = h('div.lockup');
    lockup.append(svg(tagLockup()));
    const main = options.scroll ? h('div.side-scroll.scroll', content) : content;
    content.classList.add('side-main');
    const side = h('div.side', h('div.sheen'), h('div.side-head', mark, back), main,
                   h('div.side-foot.body', 'v' + shell.info.version));
    const el = h('div', {style : {position : 'absolute', inset : '0'}}, ambient.el,
                 h('div.split', side, h('div.feature', media.el, lockup)));
    return {
        el,
        back,
        dispose() {
            media.dispose();
            ambient.dispose();
        }
    };
}

export function defaultButton(root: HTMLElement, pick: () => HTMLButtonElement | null) {
    root.addEventListener('keydown', e => {
        if (e.key !== 'Enter' || e.defaultPrevented) return;
        const button = pick();
        if (button && !button.disabled && button.offsetParent) {
            e.preventDefault();
            button.click();
        }
    });
}
