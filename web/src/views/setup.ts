import {on, rpc, messageOf} from '../bridge';
import {h, icon, show, tracked} from '../dom';
import {CheckBox, Progress} from '../controls';
import {shell, type View} from '../shell';
import {defaultButton, sideLayout} from './side';

export function setupView(): View {
    const existing = shell.info.registeredDirectory;
    const folder = shell.info.defaultInstallDirectory;
    const target = h('div.body', {style : {fontSize : '14px', color : 'var(--dim)', marginTop : '8px'}}, folder);
    rpc<{target : string | null}>('setup.resolveTarget', {folder})
        .then(r => {
            if (r.target) target.textContent = r.target;
        })
        .catch(() => {});
    const desktop = new CheckBox('Create a desktop shortcut', true);
    const startMenu = new CheckBox('Add to the Start menu', true);
    desktop.el.style.marginTop = '22px';
    startMenu.el.style.marginTop = '12px';
    const installLabel = h('span', existing ? 'Reinstall' : 'Install');
    const install = h('button.btn.primary.big', {type : 'button', style : {marginTop : '30px'}}, icon(''),
                      installLabel) as HTMLButtonElement;
    const progressText = h('div.body', {style : {fontSize : '14px', marginTop : '8px'}});
    const progress = h('div.hidden', {style : {marginTop : '18px'}}, new Progress().el, progressText);
    const error = h('div.body.hidden', {style : {color : 'var(--bad)', marginTop : '14px'}});
    const openInstalled = h('button.link', {type : 'button', style : {marginTop : '16px', alignSelf : 'flex-start'}},
                            'Open the installed launcher instead') as HTMLButtonElement;
    show(openInstalled, existing !== null);
    const content = h(
        'div', h('div.eyebrow.accent', tracked('SETUP')),
        h('div.title', {style : {margin : '6px 0 8px'}}, existing ? 'Repair or reinstall' : 'Install RWND Parabellum'),
        h('div.body',
          'Your Parabellum game files stay where they are, and you only do this once. Updates install themselves.'),
        h('div.eyebrow', {style : {margin : '28px 0 8px'}}, tracked('INSTALL LOCATION')), target, desktop.el,
        startMenu.el, install, progress, error, openInstalled);
    content.style.display = 'flex';
    const layout = sideLayout(content);
    defaultButton(layout.el, () => install);

    const off = on('setup.progress', p => { progressText.textContent = p.text; });
    install.onclick = async () => {
        const setEnabled = (v: boolean) => {
            install.disabled = !v;
            desktop.disabled = !v;
            startMenu.disabled = !v;
        };
        setEnabled(false);
        show(error, false);
        show(progress, true);
        try {
            await rpc('setup.install', {folder, desktop : desktop.checked, startMenu : startMenu.checked});
        } catch (e) {
            show(progress, false);
            error.textContent = messageOf(e);
            show(error, true);
            setEnabled(true);
        }
    };
    openInstalled.onclick = () => rpc('setup.openInstalled');
    return {
        el : layout.el,
        dispose() {
            off();
            layout.dispose();
        }
    };
}
