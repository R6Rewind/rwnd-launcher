import {h, icon, show, tracked} from '../dom';

export interface ConfirmDialog {
    el: HTMLElement;
    ask(texts?: {title?: string; text?: string}): Promise<boolean>;
}

export function confirmDialog(
    texts: {eyebrow: string; glyph : string; title : string; text : string; ok : string; cancel?: string}):
    ConfirmDialog {
    const title = h('div.title', {style : {fontSize : '22px', marginTop : '6px'}}, texts.title);
    const body = h('div.body', {style : {marginTop : '4px'}}, texts.text);
    const ok = h('button.btn.primary', {type : 'button', style : {height : '42px', flex : '1'}}, texts.ok) as
               HTMLButtonElement;
    const cancel =
        h('button.btn.glass',
          {type : 'button', style : {height : '42px', flex : '1', marginLeft : '10px', borderRadius : '8px'}},
          texts.cancel ?? 'Cancel')
    const el = h('div.ob-modal.hidden',
                 h('div.ob-dialog.glass-strong',
                   h('div.ob-head', icon(texts.glyph, 'ob-icon'), h('div.eyebrow.accent', tracked(texts.eyebrow))),
                   title, body, h('div', {style : {display : 'flex', marginTop : '22px'}}, ok, cancel)));
    return {
        el,
        ask(override) {
            if (override?.title !== undefined) title.textContent = override.title;
            if (override?.text !== undefined) body.textContent = override.text;
            show(el, true);
            ok.focus();
            return new Promise<boolean>(resolve => {
                const done = (v: boolean) => {
                    show(el, false);
                    ok.onclick = cancel.onclick = null;
                    resolve(v);
                };
                ok.onclick = () => done(true);
                cancel.onclick = () => done(false);
            });
        },
    };
}
