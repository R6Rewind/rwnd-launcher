import {rpc} from '../bridge';
import {h, icon, tracked} from '../dom';

export const LegalVersion = '2026-11-23';
export const openLegal = (page: 'terms'|'privacy') => rpc('shell.openWebsite', {path : `legal/${page}`});

export function legalDialog(onAgree: () => void): HTMLElement {
    const link = (label: string, page: 'terms'|'privacy') => {
        const button =
            h('button.btn.glass', {type : 'button', style : {height : '42px', flex : '1', borderRadius : '8px'}},
              label) as HTMLButtonElement;
        button.onclick = () => { void openLegal(page); };
        return button;
    };
    const terms = link('Terms of Service', 'terms'), privacy = link('Privacy Policy', 'privacy');
    privacy.style.marginLeft = '10px';
    const agree = h('button.btn.primary', {type : 'button', style : {height : '42px', marginTop : '10px'}},
                    'I agree') as HTMLButtonElement;
    agree.onclick = onAgree;
    return h('div.ob-modal', {style : {zIndex : '49'}},
             h('div.ob-dialog.glass-strong',
               h('div.ob-head', icon('', 'ob-icon'), h('div.eyebrow.accent', tracked('TERMS AND PRIVACY'))),
               h('div.title', {style : {fontSize : '22px', marginTop : '6px'}}, 'Before you play'),
               h('div.body', {style : {marginTop : '4px'}},
                 'By using RWND you agree to our Terms of Service and Privacy Policy.'),
               h('div', {style : {display : 'flex', marginTop : '22px'}}, terms, privacy), agree));
}

export async function showLegalNotice(): Promise<void> {
    let accepted = '';
    try {
        accepted = (await rpc<{accepted : string}>('legal.state')).accepted;
    } catch {
    }
    if (accepted === LegalVersion) return;
    await new Promise<void>(resolve => {
        const el = legalDialog(() => {
            const agree = el.querySelector<HTMLButtonElement>('.btn.primary');
            if (agree) agree.disabled = true;
            rpc('legal.accept', {version : LegalVersion})
                .then(() => {
                    el.remove();
                    resolve();
                })
                .catch(() => {
                    if (agree) agree.disabled = false;
                });
        });
        document.getElementById('root')!.append(el);
        el.querySelector<HTMLButtonElement>('.btn.primary')?.focus();
    });
}
