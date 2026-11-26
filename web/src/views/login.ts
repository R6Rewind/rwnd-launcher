import {rpc, isCanceled, messageOf, type Call} from '../bridge';
import {h, icon, show, svg, tracked} from '../dom';
import {discordGlyph, steamGlyph} from '../brand';
import {avatar, Progress} from '../controls';
import {shell, type Snapshot, type View} from '../shell';
import {defaultButton, sideLayout} from './side';

export function loginView(adding: boolean, reauthenticate?: string): View {
    const heading = h('div.title', {style : {fontSize : '34px', margin : '6px 0 8px'}});
    const lead = h('div.body');
    const discordLabel = h('span', 'Continue with Discord');
    const discord = h('button.btn.discord', {type : 'button', style : {marginTop : '30px'}}) as HTMLButtonElement;
    discord.append(svg(discordGlyph()), discordLabel);
    const steam = h('button.btn.steam.hidden', {type : 'button', style : {marginTop : '12px'}}) as HTMLButtonElement;
    steam.append(svg(steamGlyph()), h('span', 'Continue with Steam'));
    const cancel =
        h('button.link', {type : 'button', style : {flex : 'none', marginLeft : '8px'}}, 'Cancel') as HTMLButtonElement;
    const waitingText = h('div.body', {style : {fontSize : '15px', flex : '1'}},
                          'Finish signing in on the Discord page in your browser.');
    const waiting =
        h('div.hidden', {style : {marginTop : '18px'}}, new Progress().el,
          h('div', {style : {display : 'flex', marginTop : '10px', alignItems : 'flex-start'}}, waitingText, cancel));
    const error = h('div.body.hidden', {style : {color : 'var(--bad)', marginTop : '14px'}});
    const knownList = h('div.known-list');
    const known = h('div.hidden', {style : {marginTop : '34px'}},
                    h('div.known-head', h('div.rule'), h('span.eyebrow', tracked('ACCOUNTS ON THIS PC'))), knownList);
    const content = h('div', h('div.eyebrow.accent', tracked('OPERATION PARA BELLUM')), heading, lead, discord, steam,
                      waiting, error, known);
    const layout = sideLayout(content);
    defaultButton(layout.el, () => discord);
    let signIn: Call<{remembered : boolean}>|null = null;
    let snapshot: Snapshot|null = null, steamOn = false, disposed = false;

    const showError = (text: string) => {
        error.textContent = text;
        show(error, true);
    };
    const setEnabled = (enabled: boolean) => {
        discord.disabled = !enabled;
        steam.disabled = !enabled;
        layout.back.disabled = !enabled;
        for (const b of knownList.querySelectorAll('button')) (b as HTMLButtonElement).disabled = !enabled;
    };

    const describe = () => {
        if (!snapshot) return;
        const target = reauthenticate ? snapshot.accounts.find(a => a.playerId === reauthenticate) : undefined;
        const [title, text] = target ? ['Sign in again', steamOn ? `${target.displayName}'s login expired. Sign in with the same account to continue.`
                : `${target.displayName}'s login expired. Sign in with the same Discord account to continue.`]
            : adding ? ['Add an account', steamOn ? "Switch accounts in your browser first if you're adding someone else."
                : "Switch Discord accounts in your browser first if you're adding someone else."]
            : snapshot.accounts.length === 0 ? ['Sign in to play', steamOn ? 'Sign in to play Parabellum on RWND.' : 'Use your Discord account to play Parabellum on RWND.']
            : ['Welcome back', steamOn ? 'Choose an account, or sign in.' : 'Choose an account, or sign in with Discord.'];
        heading.textContent = title;
        lead.textContent = text;
    };

    async function signInAs(expected?: string, provider: 'discord'|'steam' = 'discord') {
        if (shell.busy) return;
        shell.busy = true;
        setEnabled(false);
        show(error, false);
        waitingText.textContent = provider === 'steam' ? 'Finish signing in on the Steam page in your browser.'
                                                       : 'Finish signing in on the Discord page in your browser.';
        show(waiting, true);
        try {
            signIn = provider === 'steam' ? rpc('accounts.signIn', {expected : expected ?? null, provider})
                                          : rpc('accounts.signIn', {expected : expected ?? null});
            const result = await signIn;
            shell.busy = false;
            await shell.afterSignIn();
            if (!result.remembered)
                shell.showToast(
                    "Signed in for this launcher session. Remembered login isn't enabled on the server yet.");
        } catch (e) {
            if (isCanceled(e))
                showError('Sign-in was cancelled or timed out. Try again.');
            else
                showError(messageOf(e));
        } finally {
            signIn = null;
            shell.busy = false;
            show(waiting, false);
            setEnabled(true);
        }
    }

    discord.onclick = () => signInAs(reauthenticate);
    steam.onclick = () => signInAs(reauthenticate, 'steam');
    cancel.onclick = () => signIn?.cancel();
    layout.back.onclick = () => shell.showHome();

    const providers = rpc<{steam : boolean}>('accounts.providers');
    providers.then(p => {
        if (disposed || p?.steam !== true) return;
        steamOn = true;
        show(steam, true);
        describe();
    }, () => {});

    (async () => {
        snapshot = await rpc<Snapshot>('session.snapshot');
        const target = reauthenticate ? snapshot.accounts.find(a => a.playerId === reauthenticate) : undefined;
        show(layout.back, snapshot.accounts.length > 0);
        describe();
        if (target) discordLabel.textContent = 'Sign in as ' + target.displayName;
        if (!adding && !target) {
            for (const account of snapshot.accounts) {
                const detail = account.inGame ? 'In game' : !account.hasRefresh ? 'Sign-in needed' : 'Ready to play';
                const row = h('button.row-button', {type : 'button'},
                              h('div.account-row', avatar(40, account.displayName, account.avatarUrl),
                                h('div.who', {style : {marginLeft : '12px'}},
                                  h('div.strong.nowrap', {style : {fontSize : '17px'}}, account.displayName),
                                  h('div.body', {style : {fontSize : '14px'}}, detail)),
                                icon('', 'muted-icon')));
                (row.lastElementChild!.lastElementChild as HTMLElement).style.color = 'var(--muted)';
                row.onclick = async () => {
                    await rpc('accounts.select', {id : account.playerId});
                    const now = await rpc<Snapshot>('session.snapshot');
                    if (now.accounts.find(a => a.playerId === account.playerId)?.hasRefresh)
                        await shell.afterSignIn();
                    else if (steamOn)
                        shell.showLogin(false, account.playerId);
                    else
                        await signInAs(account.playerId);
                };
                knownList.append(row);
            }
            show(known, knownList.children.length > 0);
        }
    })();

    return {
        el : layout.el,
        dispose() {
            disposed = true;
            providers.cancel();
            signIn?.cancel();
            layout.dispose();
        }
    };
}
