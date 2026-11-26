import {on, rpc} from '../bridge';
import {h, icon, show, svg, tracked} from '../dom';
import {steamGlyph} from '../brand';
import {CheckBox, textBox, tooltip} from '../controls';

export interface SteamSignIn {
    qr?: boolean;
    user?: string;
    password?: string;
    remember: boolean;
}
export interface DialogTexts {
    eyebrow: string;
    glyph: string;
    title: string;
    text: string;
}
const OUTBREAK_TEXTS: DialogTexts = {
    eyebrow : 'OUTBREAK',
    glyph : '',
    title : 'Download Outbreak',
    text : '4.4 GB from Steam. Sign in once.'
};

export interface OutbreakDialog {
    el: HTMLElement;
    open(texts?: Partial<DialogTexts>): void;
    openWaiting(texts?: Partial<DialogTexts>): void;
    close(): void;
    readonly isOpen: boolean;
    started(): void;
    error(text: string): void;
    ask(kind: string, email: string|null, retry: boolean, texts?: Partial<DialogTexts>): void;
    qr(rows: string[], texts?: Partial<DialogTexts>): void;
    passwordMode(): void;
    previewSteam(): void;
}

export function outbreakDialog(signIn: (choice: SteamSignIn) => void, cancel: () => void,
                               texts: DialogTexts = OUTBREAK_TEXTS): OutbreakDialog {
    const steamButton = (label: string) => {
        const b = h('button.btn.steam', {type : 'button', style : {marginTop : '18px'}}) as HTMLButtonElement;
        b.append(svg(steamGlyph()), h('span', label));
        return b;
    };
    const remember = new CheckBox('Remember my Steam sign-in on this PC', false);
    remember.el.style.marginTop = '16px';
    const qrButton = steamButton('Sign in with the Steam app');
    tooltip(qrButton, 'Scan a code with the Steam Mobile app. No password needed.');
    const usePassword = h('button.btn.glass', {type : 'button', style : {marginTop : '10px', height : '44px'}},
                          'Steam username and password') as HTMLButtonElement;
    const user = textBox();
    const password = textBox('', {password : true});
    const passwordButton = steamButton('Sign in');
    const useQr =
        h('button.link', {type : 'button', style : {marginTop : '12px', fontSize : '14px', alignSelf : 'flex-start'}},
          'Use the Steam app instead') as HTMLButtonElement;
    const qrChoice = h('div.ob-col', qrButton, usePassword);
    const passwordChoice =
        h('div.ob-col.hidden', h('div.eyebrow', {style : {margin : '16px 0 8px'}}, tracked('STEAM ACCOUNT NAME')),
          user.box, h('div.eyebrow', {style : {margin : '12px 0 8px'}}, tracked('PASSWORD')), password.box,
          passwordButton, useQr);
    const form = h('div.ob-col', remember.el, qrChoice, passwordChoice);

    const qrCanvas = h('canvas') as HTMLCanvasElement;
    const qrPanel = h('div.ob-col.hidden', {style : {marginTop : '18px'}}, h('div.qr-frame', qrCanvas),
                      h('div.body', {style : {marginTop : '12px', alignSelf : 'center'}},
                        'Steam app: Steam Guard, then Scan a QR code.'));

    const promptText = h('div.strong', {style : {fontSize : '16px'}});
    const code = textBox('', {big : true, maxLength : 12});
    const answerPassword = textBox('', {password : true});
    const submit = h('button.btn.primary', {type : 'button', style : {height : '42px', marginTop : '10px'}},
                     'Submit') as HTMLButtonElement;
    const codeInput = h('div.ob-col', {style : {marginTop : '12px'}}, code.box, answerPassword.box, submit);
    const promptPanel = h('div.ob-col.hidden', {style : {marginTop : '18px'}}, promptText, codeInput);
    const status = h('div.body', {style : {marginTop : '16px', minHeight : '20px'}});
    const close =
        h('button.link', {type : 'button', style : {fontSize : '14px', marginTop : '18px', alignSelf : 'flex-start'}},
          'Cancel') as HTMLButtonElement;
    const closeX = h('button.chrome-button.ob-close', {type : 'button', title : 'Close'}, '') as HTMLButtonElement;

    const headIcon = icon(texts.glyph, 'ob-icon');
    const headText = h('div.eyebrow.accent', tracked(texts.eyebrow));
    const title = h('div.title', {style : {fontSize : '22px', marginTop : '6px'}}, texts.title);
    const lead = h('div.body', {style : {marginTop : '4px'}}, texts.text);
    const dialog = h('div.ob-dialog.glass-strong', closeX, h('div.ob-head', headIcon, headText), title, lead, form,
                     qrPanel, promptPanel, status, close);
    const el = h('div.ob-modal.hidden', dialog);

    let open = false, waiting = false, answering: 'code'|'password' = 'code';
    const offs: (() => void)[] = [];
    function setTexts(t?: Partial<DialogTexts>) {
        const all = {...texts, ...(t ?? {})};
        headIcon.textContent = all.glyph;
        headText.textContent = tracked(all.eyebrow);
        title.textContent = all.title;
        lead.textContent = all.text;
    }
    function reset() {
        show(form, true);
        show(qrPanel, false);
        show(promptPanel, false);
        show(qrChoice, true);
        show(passwordChoice, false);
        status.textContent = '';
        status.style.color = '';
        password.input.value = '';
        code.input.value = '';
        answerPassword.input.value = '';
        waiting = false;
    }
    function begin(choice: SteamSignIn, text: string) {
        if (waiting) return;
        waiting = true;
        show(form, false);
        status.textContent = text;
        signIn(choice);
    }
    usePassword.onclick = () => {
        show(qrChoice, false);
        show(passwordChoice, true);
        user.input.focus();
    };
    useQr.onclick = () => {
        show(passwordChoice, false);
        show(qrChoice, true);
    };
    qrButton.onclick = () => begin({qr : true, remember : remember.checked}, 'Getting a code from Steam...');
    passwordButton.onclick = () => {
        if (!user.input.value.trim()) {
            user.input.focus();
            return;
        }
        begin({user : user.input.value.trim(), password : password.input.value, remember : remember.checked},
              'Signing in to Steam...');
        password.input.value = '';
    };
    password.input.addEventListener('keydown', e => {
        if (e.key === 'Enter') passwordButton.click();
    });
    const send = () => {
        const text = answering === 'password' ? answerPassword.input.value : code.input.value.trim().toUpperCase();
        if (!text) return;
        rpc('steam.answer', {text});
        code.input.value = '';
        answerPassword.input.value = '';
        show(promptPanel, false);
        status.textContent = 'Checking...';
    };
    submit.onclick = send;
    code.input.addEventListener('keydown', e => {
        if (e.key === 'Enter') send();
    });
    answerPassword.input.addEventListener('keydown', e => {
        if (e.key === 'Enter') send();
    });
    const dismiss = () => {
        if (waiting) cancel();
        api.close();
    };
    close.onclick = dismiss;
    closeX.onclick = dismiss;

    function showPrompt(kind: string, email: string|null, retry: boolean) {
        show(qrPanel, false);
        show(form, false);
        if (kind === 'mobileApproval') {
            show(promptPanel, false);
            status.textContent = 'Approve the sign-in in the Steam app.';
            return;
        }
        answering = kind === 'password' ? 'password' : 'code';
        show(code.box, answering === 'code');
        show(answerPassword.box, answering === 'password');
        promptText.textContent = (retry ? 'That code didn\'t work. ' : '') +
                                 (kind === 'password'    ? 'Steam needs your password.'
                                  : kind === 'emailCode' ? `Enter the code Steam emailed${email ? ' to ' + email : ''}.`
                                                         : 'Enter the Steam Guard code from the Steam app.');
        status.textContent = '';
        show(promptPanel, true);
        (answering === 'password' ? answerPassword.input : code.input).focus();
    }
    function showQr(rows: string[]) {
        const n = rows.length, scale = 4;
        qrCanvas.width = qrCanvas.height = n * scale;
        const g = qrCanvas.getContext('2d')!;
        g.fillStyle = '#fff';
        g.fillRect(0, 0, qrCanvas.width, qrCanvas.height);
        g.fillStyle = '#000';
        rows.forEach((r, y) => [...r].forEach((c, x) => {
            if (c === '1') g.fillRect(x * scale, y * scale, scale, scale);
        }));
        show(form, false);
        show(promptPanel, false);
        show(qrPanel, true);
        status.textContent = '';
    }
    function attach() {
        open = true;
        show(el, true);
        if (offs.length === 0)
            offs.push(on('steam.prompt', p => showPrompt(p.kind, p.email, p.retry)),
                      on('steam.qr', q => showQr(q.rows)));
    }

    const api: OutbreakDialog = {
        el,
        get isOpen() { return open; },
        open(t) {
            reset();
            setTexts(t);
            attach();
            qrButton.focus();
        },
        openWaiting(t) {
            reset();
            setTexts(t);
            attach();
            waiting = true;
            show(form, false);
            status.textContent = 'Waiting for Steam...';
        },
        close() {
            open = false;
            show(el, false);
            offs.splice(0).forEach(off => off());
            reset();
        },
        started() { api.close(); },
        error(text: string) {
            waiting = false;
            show(form, true);
            show(qrPanel, false);
            show(promptPanel, false);
            status.textContent = text;
            status.style.color = 'var(--bad)';
        },
        ask(kind, email, retry, t) {
            if (!open) api.openWaiting(t);
            showPrompt(kind, email, retry);
        },
        qr(rows, t) {
            if (!open) api.openWaiting(t);
            showQr(rows);
        },
        passwordMode() { usePassword.click(); },
        previewSteam() { api.open(); },
    };
    return api;
}
