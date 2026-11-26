import {on, rpc, isCanceled, kindOf, messageOf, RpcError} from '../bridge';
import {h, icon, show, svg, tracked} from '../dom';
import {rBlock} from '../brand';
import {AmbientBackdrop, MediaBackdrop} from '../backdrop';
import {avatar, CheckBox, Progress, textBox, Toggle, tooltip} from '../controls';
import {shell, type Account, type Release, type Snapshot, type View} from '../shell';
import {outbreakDialog, type SteamSignIn} from './outbreak';
import {SIZE_INFO, parseSize, sizeView, type SizeView} from './sizePage';
import {messagesView, type ChatMessage, type ChatState} from './messages';
import {confirmDialog} from './dialogs';
import {openLegal} from './legal';

const REPAIR_MESSAGE = 'Game files need a repair.';
const SMART_APP_CONTROL_MESSAGE = 'Windows Smart App Control is blocking RWND. Turn it off to play.';

export interface Profile {
    id: string;
    displayName: string;
    avatarUrl: string|null;
    role: string;
    rating: {season: string; rating : number; deviation : number; wins : number; losses : number; rank : string};
    progression: {level: number; experience : number}|null;
    wallet: {renown: number; credits : number}|null;
}
export interface Announcement {
    id: string;
    title: string;
    body: string;
    category: string;
    banner: boolean;
    severity: string;
    publishAt: string;
    publishDate: string;
    publishTicks: number;
}
export interface Person {
    username: string;
    avatarUrl: string|null;
    online: boolean;
}
export interface FriendLink {
    person: Person|null;
    direction: string;
}
export interface FriendList {
    friends: FriendLink[];
    requests: FriendLink[];
}
export interface MatchPresence {
    map: string;
    playlist: string;
    round: number|null;
    totalRounds: number|null;
    score: number|null;
    kills: number;
    deaths: number;
}
export interface PresencePlayer {
    displayName: string;
    avatarUrl: string|null;
    activity: string;
    match: MatchPresence|null;
}
export interface PresenceFeed {
    onlinePlayers: number;
    players: PresencePlayer[];
}
export interface ServerChoice {
    available: boolean;
    regions?: {id: string; name : string}[];
    allowed?: string[];
}
const REGION_NAMES: Record<string, string> = {
    na : 'North America',
    eu : 'Europe'
};

export interface HomeView extends View {
    showSample(profile: Profile, news: Announcement[], friends: FriendList, presence: PresenceFeed): void;
    openAccountMenu(): Promise<void>;
    openSettings(): void;
    previewOutbreakSteam(): void;
    previewOutbreakProgress(): void;
    previewGear(hover: boolean): void;
    previewGearMenu(): void;
    previewSize(): SizeView;
    previewServersOpen(): void;
    previewVote(title: string, question: string|null): void;
    previewLaunchProgress(title: string, detail: string): void;
    previewMessages(state: ChatState, open: string, history: ChatMessage[], draft?: string): void;
    ready: Promise<void>;
}

const colors = {
    good : 'var(--good)',
    accent : 'var(--accent)',
    dim : 'var(--dim)',
    muted : 'var(--muted)'
};

export function homeView(openSettingsFirst: boolean): HomeView {
    const media = new MediaBackdrop(18);
    const ambient = new AmbientBackdrop(media);
    let snapshot: Snapshot|null = null;
    let profileShown: string|null = null;
    let friends: FriendList|null = null;
    let friendsNote: string|null = null;
    let presence: PresenceFeed|null = null;
    let searchResults: Person[]|null = null;
    let searchedFor = '';
    let searchNote: string|null = null;
    let stopArmed = false;
    let disarmTimer = 0, searchTimer = 0;
    let disposed = false;
    const selected = () => snapshot?.selected ?? null;
    const needsOutbreak = () => !!snapshot?.outbreak?.live && !snapshot.outbreak.ready;
    let outbreakCall: {cancel(): void}|null = null;
    let outbreakPercent: number|null = null;
    let outbreakAutoTried = false;
    const selectedAccount = () => snapshot?.accounts.find(a => a.playerId === snapshot?.selected) ?? null;

    const accountAvatarHost = h('div');
    const markSmall = h('div.mark');
    markSmall.append(svg(rBlock()));
    const switcher = h('button.row-button.switcher.no-drag', {type : 'button'}, markSmall,
                       h('div.brand', h('div.rwnd', 'RWND'), h('div.sub', 'Parabellum')),
                       h('div', {style : {display : 'flex', alignItems : 'center'}}, accountAvatarHost,
                         h('div.chevrons', icon(''), icon('')))) as HTMLButtonElement;
    tooltip(switcher, () => (snapshot?.accountLimit ?? 1) > 1 ? 'Switch account' : 'Account');

    const nav = (glyph: string, label: string) =>
        h('button.nav-item', {type : 'button'}, icon(glyph), h('span.label', label)) as HTMLButtonElement;
    const navHome = nav('', 'Home'), navNews = nav('', 'News'), navLeaderboard = nav('', 'Leaderboard'),
          navSettings = nav('', 'Settings');
    const messages = messagesView(() => {
        if (friends) renderSocial();
    });
    const navMessages = nav('\uE8BD', 'Messages');
    navMessages.append(messages.badge);
    const addAccount = h('button.chrome-button.small', {type : 'button'}, '') as HTMLButtonElement;
    tooltip(addAccount, 'Add an account');
    const accountsHeading = h('div.body', 'My accounts');
    const accountList = h('div.account-list');
    const accountsSection = h('div.hidden', h('div.separator', {style : {margin : '8px 4px 14px'}}),
                              h('div.accounts-head', accountsHeading, addAccount), accountList);
    const left = h('div.left', h('div.sheen'),
                   h('div.left-dock', switcher, h('div.separator', {style : {margin : '10px 4px 12px'}}),
                     h('div', navHome, navNews, navMessages, navLeaderboard, navSettings), accountsSection,
                     h('div.version-label.body', 'RWND Parabellum v' + shell.info.version)));

    const tabGeneral = h('button.tab', {type : 'button'}, h('span', 'General'), h('div.bar')) as HTMLButtonElement;
    const tabNews = h('button.tab', {type : 'button'}, h('span', 'News'), h('div.bar')) as HTMLButtonElement;
    const tabBar = h('div.tabbar.no-drag', tabGeneral, tabNews);
    const settingsHeading = h('div.title.settings-heading.hidden', 'Settings');

    const featureMark = h('div.badge');
    featureMark.append(svg(rBlock()));
    const featureOnline = h('span.strong');
    const featureFaces = h('div.faces');
    const playIcon = icon('');
    const playLabel = h('span', tracked('PLAY'));
    const playButton = h('button.btn.primary.play', {type : 'button'}, playIcon, playLabel) as HTMLButtonElement;
    const launchTitle = h('div.strong');
    const launchProgress = new Progress();
    show(launchProgress.el, false);
    const launchDetail = h('div.body');
    const gearButton = h('button.btn.play-gear', {type : 'button'}, icon('')) as HTMLButtonElement;
    tooltip(gearButton, 'Game files');
    const outbreakText = h('span');
    const outbreakGet = h('button.link', {type : 'button', style : {fontSize : '14px', marginLeft : '8px'}}, 'Get') as
                        HTMLButtonElement;
    const outbreakLine = h('div.ob-line.hidden', icon(''), outbreakText, outbreakGet);
    const serversValue = h('span.servers-value', 'Global');
    const serversButton =
        h('button.servers-box', {type : 'button'}, serversValue, icon('\uE70D', 'servers-chev')) as HTMLButtonElement;
    const serversList = h('div.servers-list.glass-strong.hidden');
    const servers = h('div.servers.hidden', h('span.eyebrow', tracked('SERVERS')), serversButton, serversList);
    const featured =
        h('div.featured', h('div.shadow'), h('div.clip', media.el), h('div.fade-x'), h('div.fade-y'),
          h('div.edge-line'), featureMark, h('div.online', icon(''), featureOnline, featureFaces),
          h('div.hero', h('div.display', 'PARA BELLUM'), h('div.body.tagline', 'Queue Quick Play and Ranked on RWND.'),
            h('div.play-row', h('div.play-group', playButton, gearButton),
              h('div.launch-info', launchTitle, launchProgress.el, launchDetail, outbreakLine))),
          servers);

    const seasonTitle = h('div.title', 'Your season');
    const seasonChipText = h('span');
    const seasonChip = h('div.chip.hidden', seasonChipText);
    const leaderboardLink =
        h('button.link', {type : 'button'}, h('span', 'Leaderboard'), icon('', 'chev')) as HTMLButtonElement;
    const stat = (label: string) => {
        const value = h('div.title', '-'), sub = h('div.body');
        return {card : h('div.glass.stat', h('div.eyebrow', tracked(label)), value, sub), value, sub};
    };
    const rank = stat('RANK'), rating = stat('RATING'), record = stat('RECORD'), level = stat('LEVEL');
    rank.value.classList.add('nowrap');
    const generalPage =
        h('div.page.scroll', featured, h('div.season-head', seasonTitle, seasonChip, h('div.spacer'), leaderboardLink),
          h('div.stats', rank.card, rating.card, record.card, level.card));
    const newsList = h('div');
    const newsPage = h('div.page.scroll.hidden', newsList);

    const instancesText = h('div.body.nowrap');
    const messageToggle = new Toggle(() => saveToggles());
    const friendToggle = new Toggle(() => saveToggles()), updateToggle = new Toggle(() => saveToggles()),
          trayToggle = new Toggle(() => saveToggles());
    const startupToggle = new Toggle(v => setStartup(v));
    const mouse = textBox('0.02', {maxLength : 10});
    mouse.box.classList.add('mouse-box');
    const mouseHint = h('div.body', 'Default 0.02');
    const mouseReset =
        h('button.btn.glass', {type : 'button', style : {marginLeft : '8px'}}, icon('\uE72C')) as HTMLButtonElement;
    tooltip(mouseReset, 'Back to 0.02');
    (mouseReset.firstElementChild as HTMLElement).style.margin = '0';
    const fpsToggle = new Toggle(on => setFps(on));
    const fps = textBox('144', {maxLength : 4});
    fps.box.classList.add('fps-box');
    const fpsHint = h('div.body', 'Off');
    const startupText = h('div.body', 'Open RWND quietly in the tray when you sign in to Windows.');
    const versionText = h('div.strong');
    const updateText = h('div.body');
    const updateButton = h('button.btn.glass', {type : 'button'}, 'Check for updates') as HTMLButtonElement;
    const installText = h('div.body.nowrap');
    const uninstallRow = h('div');
    const glassButton = (label: string, glyph?: string) =>
        h('button.btn.glass', {type : 'button'}, glyph ? icon(glyph) : null, h('span', label)) as HTMLButtonElement;
    const openInstances = glassButton('Open'), openInstall = glassButton('Open'), uninstall = glassButton('Uninstall');
    const openTerms = glassButton('Open'), openPrivacy = glassButton('Open');
    openTerms.onclick = () => { void openLegal('terms'); };
    openPrivacy.onclick = () => { void openLegal('privacy'); };
    const verifyGame = glassButton('Verify');
    uninstall.style.color = 'var(--bad)';
    const setting = (title: string|HTMLElement, detail: HTMLElement|string, action: HTMLElement, extra?: HTMLElement,
                     tall = false) => h('div.setting' + (tall ? '.tall' : ''),
                                        h('div.text', typeof title === 'string' ? h('div.strong', title) : title,
                                          typeof detail === 'string' ? h('div.body', detail) : detail, extra ?? null),
                                        action);
    const rule = () => h('div.setting-rule');
    uninstallRow.append(rule(), setting('Uninstall RWND Parabellum',
                                        'Removes the launcher. Your game folder and saved logins stay.', uninstall));
    const settingsPage =
        h('div.page.scroll.hidden',
          h('div.settings', h('span.eyebrow', tracked('GAME')),
            h('div.glass.settings-group', setting('Verify game files', 'Fixes damaged or missing files.', verifyGame),
              rule(), setting('Account game copies', instancesText, openInstances)),
            h('span.eyebrow.later', tracked('NOTIFICATIONS')),
            h('div.glass.settings-group',
              setting('Friend requests', 'Show a Windows notification when someone sends you a friend request.',
                      friendToggle.el),
              rule(), setting('Messages', 'Show a Windows notification for new messages.', messageToggle.el), rule(),
              setting('Launcher updates', 'Show a Windows notification when a new launcher version is ready.',
                      updateToggle.el),
              rule(),
              setting('Keep running in the tray',
                      'Closing the window keeps RWND in the notification area so notifications still reach you.',
                      trayToggle.el),
              rule(), setting('Start with Windows', startupText, startupToggle.el)),
            h('span.eyebrow.later', tracked('LAUNCHER')),
            h('div.glass.settings-group', setting(versionText, updateText, updateButton), rule(),
              setting('Install location', installText, openInstall), uninstallRow),
            h('span.eyebrow.later', tracked('LEGAL')),
            h('div.glass.settings-group', setting('Terms of Service', 'The rules for using RWND.', openTerms), rule(),
              setting('Privacy Policy', 'What RWND collects and why.', openPrivacy))));
    const messagesPage = messages.page;
    const center = h('div.center', h('div', {style : {display : 'grid'}}, tabBar, settingsHeading), generalPage,
                     newsPage, settingsPage, messagesPage);
    settingsHeading.style.gridArea = tabBar.style.gridArea = '1 / 1';
    for (const p of [generalPage, newsPage, settingsPage, messagesPage]) p.style.gridArea = '2 / 1';

    const friendsTab = h('button.tab', {type : 'button'}, h('span', 'Friends'), h('div.bar')) as HTMLButtonElement;
    const onlineTab = h('button.tab', {type : 'button'}, h('span', 'Online now'), h('div.bar')) as HTMLButtonElement;
    const searchInput =
        h('input', {type : 'text', placeholder : 'Search or add players', spellcheck : false, autocomplete : 'off'}) as
        HTMLInputElement;
    const socialList = h('div', {id : 'social-list'});
    const right = h('div.right', h('div.sheen'),
                    h('div.right-dock', h('div.social-tabs', friendsTab, onlineTab),
                      h('div.search', icon(''), searchInput), h('div.social-scroll.scroll', socialList)));

    const menuHeader = h('span.eyebrow', tracked('ACCOUNTS'));
    const menuAccounts = h('div.menu-accounts');
    const addAccountLabel = h('span.strong', 'Add another account');
    const addAccountItem =
        h('button.row-button', {type : 'button'}, h('div.item', icon(''), addAccountLabel)) as HTMLButtonElement;
    (addAccountItem.querySelector('.icon') as HTMLElement).style.color = 'var(--accent)';
    const signOutLabel = h('span.strong');
    const signOutItem =
        h('button.row-button', {type : 'button'}, h('div.item', icon(''), signOutLabel)) as HTMLButtonElement;
    (signOutItem.querySelector('.icon') as HTMLElement).style.color = 'var(--muted)';
    const menu = h('div.menu.glass-strong.hidden', menuHeader, menuAccounts, h('div.sep'), addAccountItem, signOutItem);
    const menuCatcher = h('div.menu-catcher.hidden');

    const gearItem = (glyph: string, label: string) =>
        h('button.row-button', {type : 'button'}, h('div.item', icon(glyph), h('span.strong', label))) as
        HTMLButtonElement;
    const sizeValue = h('span.body.gear-value');
    const sizeItem = gearItem('\uE7B8', 'Download size');
    sizeItem.firstElementChild!.append(sizeValue);
    const verifyItem = gearItem('\uE90F', 'Verify and repair');
    const openGameItem = gearItem('\uE838', 'Open game folder');
    const changeGameItem = gearItem('', 'Change game folder');
    const gearClose =
        h('button.chrome-button.ob-close', {type : 'button', title : 'Close'}, '\uE8BB') as HTMLButtonElement;
    const gearMenu = h(
        'div.ob-modal.hidden',
        h('div.ob-dialog.glass-strong.gear-panel', gearClose, h('span.eyebrow.accent', tracked('GAME FILES')), sizeItem,
          h('div.sep'), verifyItem, openGameItem, changeGameItem, h('div.sep'), h('span.eyebrow', tracked('MOUSE')),
          h('div.gear-mouse',
            h('div', {style : {flex : '1', minWidth : '0'}}, h('div.strong', 'Sensitivity multiplier'), mouseHint),
            mouse.box, mouseReset),
          h('div.sep'), h('span.eyebrow', tracked('DISPLAY')),
          h('div.gear-mouse', h('div', {style : {flex : '1', minWidth : '0'}}, h('div.strong', 'FPS limit'), fpsHint),
            fps.box, fpsToggle.el)));
    const steamDialog = outbreakDialog(choice => updateOutbreak(choice), () => outbreakCall?.cancel());
    const voteDialog = confirmDialog(
        {eyebrow : 'SEASON VOTE', glyph : '\uE73E', title : '', text : '', ok : 'Open vote', cancel : 'Later'});
    const voteAsked = new Set<string>();
    const el = h('div', {style : {position : 'absolute', inset : '0'}}, ambient.el, h('div.home', left, center, right),
                 menuCatcher, menu, gearMenu, steamDialog.el, voteDialog.el);

    const state = (a: Account) => a.inGame ? 'In game' : a.canSignInSilently ? 'Ready' : 'Sign-in needed';

    function accountRow(entry: Account, compact: boolean) {
        const s = state(entry);
        const dot = s === 'In game' ? colors.good : s === 'Ready' ? colors.accent : colors.dim;
        const row =
            h('div.account-row', avatar(compact ? 34 : 36, entry.displayName, entry.avatarUrl, dot),
              h('div.who', h('div.strong.nowrap.name', entry.displayName),
                h('div.body.state',
                  {style : {color : s === 'In game' ? colors.good
                                    : s === 'Ready' ? colors.muted
                                                    : colors.accent}},
                  s)));
        if (!compact && entry.playerId === selected()) {
            const check = icon('', 'check-mark');
            row.append(check);
        }
        const button = h('button.row-button', {type : 'button', style : {padding : '7px 8px', marginBottom : '2px'}},
                         row) as HTMLButtonElement;
        if (compact && entry.playerId === selected()) button.classList.add('selected');
        button.onclick = async () => {
            closeMenu();
            if (shell.busy || entry.playerId === selected()) return;
            snapshot = await rpc<Snapshot>('accounts.select', {id : entry.playerId});
            openServers(false);
            serverChoice = {available : false};
            renderServers();
            render();
            searchResults = null;
            searchedFor = '';
            queueSearch();
            await Promise.all([ loadProfile(), loadSocial() ]);
        };
        return button;
    }

    async function refreshAccounts() {
        snapshot = await rpc<Snapshot>('session.snapshot');
        if (disposed) return;
        if (snapshot.accounts.length === 0) {
            shell.showLogin(false);
            return;
        }
        render();
        const account = selectedAccount();
        if (!outbreakAutoTried && needsOutbreak() && snapshot.outbreak!.steam && !snapshot.outbreak!.installing &&
            !shell.busy && !outbreakCall && account?.canSignInSilently && !account.inGame &&
            shell.info.mode === 'launcher') {
            outbreakAutoTried = true;
            updateOutbreak();
        }
    }

    function render() {
        if (!snapshot) return;
        const account = selectedAccount();
        if (!account) return;
        accountAvatarHost.replaceChildren(avatar(28, account.displayName, account.avatarUrl));
        messages.setAccount(account.canSignInSilently ? account.playerId : null);
        accountList.replaceChildren(...snapshot.accounts.map(a => accountRow(a, true)));
        const multiple = snapshot.accountLimit > 1, full = snapshot.accounts.length >= snapshot.accountLimit;
        show(accountsSection, multiple);
        menuHeader.textContent = tracked(multiple ? 'ACCOUNTS' : 'ACCOUNT');
        show(addAccount, multiple);
        show(addAccountItem, multiple);
        addAccount.disabled = full;
        menuAccounts.replaceChildren(...snapshot.accounts.map(a => accountRow(a, false)));
        addAccountItem.disabled = full;
        addAccountLabel.textContent = full ? 'Account limit reached' : 'Add another account';
        signOutLabel.textContent = 'Sign out of ' + account.displayName;
        signOutItem.disabled = account.inGame;
        if (shell.busy) return;
        if (account.inGame)
            setPlay('', stopArmed ? 'CONFIRM' : 'STOP GAME', true, `${account.displayName} is playing`,
                    stopArmed
                        ? 'Click again to close the game.'
                        : 'In game for ' + elapsed(snapshot.nowTicks - (account.gameStartTicks ?? snapshot.nowTicks)));
        else if (!account.canSignInSilently)
            setPlay('', 'SIGN IN', true, 'Sign-in needed', `${account.displayName}'s login expired.`);
        else
            setPlay('', 'PLAY', true, 'Ready',
                    `Launches as ${account.displayName}.` +
                        (snapshot.runningCount > 0 ? ` ${snapshot.runningCount} other game${
                                                         snapshot.runningCount === 1 ? ' is' : 's are'} running.`
                                                   : ''));
    }

    function renderOutbreakLine() {
        const pending = needsOutbreak() || outbreakCall !== null;
        show(outbreakLine, pending && !shell.busy);
        if (!pending) return;
        outbreakText.textContent = outbreakCall
                                       ? 'Outbreak' + (outbreakPercent !== null ? ` · ${outbreakPercent}%` : '...')
                                       : 'Outbreak is here';
        show(outbreakGet, !outbreakCall);
    }

    function setPlay(glyph: string, label: string, enabled: boolean, title: string, detail: string) {
        playIcon.textContent = glyph;
        playLabel.textContent = tracked(label);
        playButton.disabled = !enabled;
        launchTitle.textContent = title;
        launchDetail.textContent = detail;
        const inGame = label === 'STOP GAME' || label === 'CONFIRM' || label === 'STOPPING';
        launchTitle.style.color = inGame && label !== 'CONFIRM' ? colors.good : 'var(--text)';
        playButton.classList.toggle('confirm', label === 'CONFIRM');
        playButton.classList.toggle('stop', inGame && label !== 'CONFIRM');
        gearButton.disabled = !(label === 'PLAY' || label === 'SIGN IN') || shell.busy;
        renderOutbreakLine();
        const trayLabel = label === 'PLAY'      ? 'Play RWND'
                          : label === 'SIGN IN' ? 'Sign in to play RWND'
                          : label === 'UPDATE'  ? 'Update RWND'
                          : label === 'PAUSE'   ? 'Updating Outbreak...'
                          : inGame              ? 'RWND is running'
                                                : 'Launching RWND...';
        shell.updateTray(selectedAccount()?.displayName ?? null, trayLabel,
                         label === 'PLAY' || label === 'SIGN IN' || label === 'UPDATE');
    }

    const elapsed = (ticks: number) => {
        const minutes = ticks / 600_000_000;
        return minutes < 1    ? 'less than a minute'
               : minutes < 60 ? `${Math.floor(minutes)} min`
                              : `${Math.floor(minutes / 60)} h ${Math.floor(minutes % 60)} min`;
    };

    function openMenu() {
        const r = switcher.getBoundingClientRect(), root = el.getBoundingClientRect();
        menu.style.left = r.left - root.left + 'px';
        menu.style.top = r.bottom - root.top + 6 + 'px';
        show(menu, true);
        show(menuCatcher, true);
        menu.animate([ {opacity : 0}, {opacity : 1} ], {duration : 150});
    }
    function closeMenu() {
        show(menu, false);
        show(menuCatcher, false);
    }
    switcher.onclick = async () => {
        await refreshAccounts();
        menu.classList.contains('hidden') ? openMenu() : closeMenu();
    };
    menuCatcher.onmousedown = () => closeMenu();
    const add = () => {
        closeMenu();
        if (!shell.busy && snapshot && snapshot.accounts.length < snapshot.accountLimit) shell.showLogin(true);
    };
    addAccount.onclick = add;
    addAccountItem.onclick = add;

    async function signOut() {
        closeMenu();
        const account = selectedAccount();
        if (shell.busy || !account) return;
        shell.busy = true;
        try {
            const result = await rpc<{revoked : boolean}>('accounts.signOut', {id : account.playerId});
            shell.showToast(
                result.revoked
                    ? `Signed out of ${account.displayName}. Game saves are kept.`
                    : `Signed out of ${
                          account
                              .displayName} on this PC. The server couldn't confirm; revoke it from account security on the website.`);
        } catch (e) {
            if (!isCanceled(e)) shell.showToast(messageOf(e));
        } finally {
            shell.busy = false;
        }
        profileShown = null;
        await refreshAccounts();
        if (snapshot && snapshot.accounts.length > 0) await Promise.all([ loadProfile(), loadSocial() ]);
    }
    signOutItem.onclick = signOut;

    function checkNav(which: 'home'|'news'|'settings'|'messages') {
        navMessages.classList.toggle('checked', which === 'messages');
        navHome.classList.toggle('checked', which === 'home');
        navNews.classList.toggle('checked', which === 'news');
        navSettings.classList.toggle('checked', which === 'settings');
    }
    function showPage(page: HTMLElement) {
        for (const p of [generalPage, newsPage, settingsPage, messagesPage]) show(p, p === page);
        const settings = page === settingsPage || page === messagesPage;
        show(tabBar, !settings);
        show(settingsHeading, settings);
        settingsHeading.textContent = page === messagesPage ? 'Messages' : 'Settings';
        messages.shown(page === messagesPage);
        if (settings) loadSettings();
    }
    function selectTab(news: boolean) {
        tabGeneral.classList.toggle('checked', !news);
        tabNews.classList.toggle('checked', news);
        checkNav(news ? 'news' : 'home');
        showPage(news ? newsPage : generalPage);
    }
    navHome.onclick = () => selectTab(false);
    navNews.onclick = () => selectTab(true);
    tabGeneral.onclick = () => selectTab(false);
    tabNews.onclick = () => selectTab(true);
    navSettings.onclick = () => {
        checkNav('settings');
        showPage(settingsPage);
    };
    const openMessages = (name?: string) => {
        checkNav('messages');
        showPage(messagesPage);
        if (name) messages.open(name);
    };
    navMessages.onclick = () => openMessages();
    const openWebsite = (path: string) => rpc('shell.openWebsite', {path});
    navLeaderboard.onclick = () => openWebsite('leaderboard');
    leaderboardLink.onclick = () => openWebsite('leaderboard');

    async function loadProfile() {
        const account = selectedAccount();
        if (!account) return;
        const id = account.playerId;
        profileShown = id;
        seasonTitle.textContent = account.displayName + "'s season";
        show(seasonChip, false);
        for (const s of [rank, rating, record, level]) {
            s.value.textContent = '-';
            s.sub.textContent = '';
        }
        if (!account.canSignInSilently) {
            rank.sub.textContent = 'Sign in to load stats';
            return;
        }
        rank.sub.textContent = 'Loading...';
        try {
            const profile = await rpc<Profile>('accounts.profile', {id});
            if (profile.id === id && !shell.busy) await refreshAccounts();
            if (profileShown === id) showProfile(profile);
            if (profileShown === id) loadServers();
            if (profileShown === id) void askVote(id);
        } catch (e) {
            if (kindOf(e) === 'loginRequired') {
                if (profileShown === id) {
                    rank.sub.textContent = 'Sign in to load stats';
                    await refreshAccounts();
                }
            } else if (kindOf(e) === 'canceled') {
            } else if (profileShown === id)
                rank.sub.textContent = 'Stats unavailable right now';
        }
    }

    async function askVote(id: string) {
        try {
            const poll = await rpc<{id : string; title : string; question : string | null}|null>('polls.pending', {id});
            if (!poll || voteAsked.has(poll.id) || profileShown !== id) return;
            voteAsked.add(poll.id);
            if (await voteDialog.ask({title : poll.title, text : poll.question || 'Pick the next season.'}))
                await openWebsite('vote');
        } catch {
        }
    }

    function showProfile(profile: Profile) {
        const r = profile.rating;
        seasonTitle.textContent = profile.displayName + "'s season";
        seasonChipText.textContent = 'Season ' + r.season;
        show(seasonChip, true);
        rank.value.textContent = r.rank.trim() ? r.rank : 'Unranked';
        rank.sub.textContent = r.wins + r.losses === 0 ? 'Play Ranked to place' : 'Ranked';
        rating.value.textContent = Math.round(r.rating).toString();
        rating.sub.textContent = `± ${Math.round(r.deviation)} uncertainty`;
        record.value.textContent = `${r.wins}-${r.losses}`;
        record.sub.textContent =
            r.wins + r.losses === 0 ? 'No matches yet' : `${Math.round(100 * r.wins / (r.wins + r.losses))}% win rate`;
        level.value.textContent = profile.progression ? String(profile.progression.level) : '-';
        level.sub.textContent = profile.wallet ? `${profile.wallet.renown.toLocaleString('en-US')} Renown` : '';
    }

    async function loadNews() {
        try {
            const items = await rpc<Announcement[]>('news.announcements');
            const sorted =
                [...items ]
                    .sort((a, b) => (Number(b.banner) - Number(a.banner)) || (b.publishTicks - a.publishTicks))
                    .slice(0, 30);
            newsList.replaceChildren(...sorted.map(newsCard));
            if (items.length === 0) newsList.append(emptyNews());
        } catch (e) {
            if (kindOf(e) !== 'canceled')
                newsList.replaceChildren(note('The news feed is temporarily unavailable. Check back shortly.'));
        }
    }

    function newsCard(item: Announcement) {
        const meta = h(
            'div.meta', h('span.eyebrow', {
                style : {
                    color : item.severity === 'Critical' || item.severity === 'Warning' ? 'var(--bad)' : 'var(--accent)'
                }
            },
                          tracked(item.category.toUpperCase()) + '   ·   ' + item.publishDate));
        if (item.banner) meta.append(h('div.chip.pinned', h('span.eyebrow', 'PINNED')));
        const more = h('button.link', {type : 'button'}, h('span', 'Read on the website'), icon('', 'chev')) as
                     HTMLButtonElement;
        more.onclick = () => openWebsite('Update/' + item.id);
        return h('div.glass.news-card', meta, h('div.title', item.title), h('div.body', item.body), more);
    }

    function emptyNews() {
        const roadmap = h('button.link', {type : 'button'}, 'Explore the roadmap') as HTMLButtonElement;
        roadmap.style.marginTop = '14px';
        roadmap.onclick = () => openWebsite('Roadmap');
        return h('div.glass.news-card', {style : {padding : '22px 24px'}},
                 h('div.eyebrow.accent', {style : {fontSize : '12px'}}, tracked('NEWSROOM · STANDING BY')),
                 h('div.title', {style : {fontSize : '22px', marginTop : '8px'}}, 'The next transmission is ahead.'),
                 h('div.body', {style : {marginTop : '8px'}},
                   'No announcements are published yet. Updates from the team will appear here and on the website.'),
                 roadmap);
    }

    const note = (text: string) => h('div.body.note', text);

    let serverChoice: ServerChoice = {available : false};
    let serversFor: string|null = null;
    let serversSaving = false;
    const serversOpen = () => !serversList.classList.contains('hidden');
    const regionName = (r: {id: string; name : string}) => REGION_NAMES[r.id] ?? r.name;

    async function loadServers() {
        const id = selected();
        const account = selectedAccount();
        if (!id || !account?.canSignInSilently) {
            serverChoice = {available : false};
            serversFor = id;
            renderServers();
            return;
        }
        if (serversSaving || serversOpen()) return;
        const choice = await rpc<ServerChoice>('servers.get', {id}).catch(() => null);
        if (!choice || disposed || id !== selected() || serversSaving || serversOpen()) return;
        serverChoice = choice;
        serversFor = id;
        renderServers();
    }

    function renderServers() {
        const regions = serverChoice.regions ?? [];
        const allowed = serverChoice.allowed ?? [];
        const serversShown = serverChoice.available && regions.length > 1 && serversFor === selected();
        show(servers, serversShown);
        featured.classList.toggle('with-servers', serversShown);
        const on = regions.filter(r => allowed.includes(r.id));
        serversValue.textContent = on.length === regions.length ? 'Global'
                                   : on.length === 1            ? on[0].name + ' only'
                                                                : on.map(r => r.name).join(' + ');
        serversList.replaceChildren(...regions.map(r => {
            const check = new CheckBox(regionName(r), allowed.includes(r.id), v => saveServers(r.id, v));
            check.disabled = serversSaving || allowed.includes(r.id) && on.length === 1;
            return h('div.servers-row', check.el);
        }));
    }

    async function saveServers(region: string, value: boolean) {
        const id = selected();
        if (!id || !serverChoice.available) return;
        const before = serverChoice;
        const allowed = (serverChoice.regions ?? [])
                            .map(r => r.id)
                            .filter(r => r === region ? value : (before.allowed ?? []).includes(r));
        if (allowed.length === 0) {
            renderServers();
            return;
        }
        serverChoice = {...before, allowed};
        serversSaving = true;
        renderServers();
        try {
            const saved = await rpc<ServerChoice>('servers.set', {id, allowed});
            if (id === selected()) serverChoice = saved;
        } catch (e) {
            if (id === selected()) serverChoice = before;
            if (kindOf(e) !== 'canceled') shell.showToast(messageOf(e));
        } finally {
            serversSaving = false;
            renderServers();
        }
    }

    function openServers(open: boolean) {
        show(serversList, open);
        serversButton.classList.toggle('open', open);
        if (open) renderServers();
    }
    serversButton.onclick = () => openServers(!serversOpen());
    const onServersOutside = (e: MouseEvent) => {
        if (serversOpen() && !servers.contains(e.target as Node)) openServers(false);
    };
    document.addEventListener('mousedown', onServersOutside);

    async function loadSocial() {
        const id = selected();
        const result =
            await rpc<{
                friends : FriendList | null; note : string | null; presence : PresenceFeed | null; canceled : boolean
            }>('social.load', {id})
                .catch(() => null);
        if (!result || result.canceled || disposed) return;
        if (id !== selected()) return;
        if (result.friends && id) notifyNewRequests(id, result.friends);
        friends = result.friends;
        friendsNote = result.note;
        presence = result.presence ?? presence;
        renderSocial();
    }

    function notifyNewRequests(id: string, list: FriendList) {
        const incoming = list.requests.filter(r => r.direction === 'Incoming' && r.person).map(r => r.person!.username);
        const seen = shell.seenRequests.get(id);
        if (seen && snapshot?.prefs.friendNotifications) {
            const fresh = incoming.filter(n => ![...seen].some(s => s.toLowerCase() === n.toLowerCase()));
            if (fresh.length === 1)
                shell.notify('New friend request', `${fresh[0]} wants to be friends on RWND.`);
            else if (fresh.length > 1)
                shell.notify('New friend requests',
                             `${fresh.slice(0, 3).join(', ')}${
                                 fresh.length > 3 ? ` and ${fresh.length - 3} more` : ''} want to be friends on RWND.`);
        }
        shell.seenRequests.set(id, new Set(incoming));
    }

    friendsTab.onclick = () => {
        setSocialTab(false);
        renderSocial();
        queueSearch();
    };
    onlineTab.onclick = () => {
        setSocialTab(true);
        renderSocial();
        queueSearch();
    };
    const setSocialTab = (online: boolean) => {
        friendsTab.classList.toggle('checked', !online);
        onlineTab.classList.toggle('checked', online);
    };
    searchInput.addEventListener('input', () => {
        renderSocial();
        queueSearch();
    });

    function queueSearch() {
        clearTimeout(searchTimer);
        if (friendsTab.classList.contains('checked') && searchInput.value.trim().length >= 2)
            searchTimer = window.setTimeout(search, 350);
        else {
            searchResults = null;
            searchedFor = '';
            searchNote = null;
        }
    }

    async function search() {
        const query = searchInput.value.trim();
        const id = selected();
        if (query.length < 2 || !id) return;
        if (!selectedAccount()?.canSignInSilently) {
            searchNote = 'Sign in to search for players.';
            renderSocial();
            return;
        }
        try {
            const people = await rpc<Person[]>('social.search', {id, query});
            if (searchInput.value.trim() !== query || id !== selected()) return;
            searchResults = people;
            searchedFor = query;
            searchNote = null;
        } catch (e) {
            if (kindOf(e) === 'canceled') return;
            searchNote = kindOf(e) === 'loginRequired' ? 'Sign in to search for players.' : messageOf(e);
        }
        renderSocial();
    }

    async function friendAction(button: HTMLButtonElement, action: string, username: string, done: string) {
        const id = selected();
        if (!id) return;
        button.disabled = true;
        try {
            await rpc('social.action', {id, action, username});
            shell.showToast(done);
            await loadSocial();
        } catch (e) {
            if (kindOf(e) === 'canceled') return;
            button.disabled = false;
            shell.showToast(messageOf(e));
        }
    }

    const activity = (p: PresencePlayer) =>
        p.activity === 'InMatch' && p.match
            ? `In match · ${p.match.map}` + (p.match.round !== null && p.match.totalRounds !== null
                                                 ? ` · R${p.match.round}/${p.match.totalRounds}`
                                                 : '')
        : p.activity === 'InMatch'     ? 'In match'
        : p.activity === 'Matchmaking' ? 'Finding a match'
        : p.activity === 'MainMenu'    ? 'In menus'
                                       : 'Online';

    function section(title: string, count: number) {
        socialList.append(h('div.section-head' + (socialList.children.length === 0 ? '.first' : ''),
                            h('div.body', title), h('div.badge', h('span.eyebrow', String(count)))));
    }

    function personRow(name: string, avatarUrl: string|null, detail: string, dot: string, dim = false) {
        const face = avatar(36, name, avatarUrl, dot);
        if (dim) face.style.opacity = '0.55';
        return h('div.person', face,
                 h('div.who', h('div.strong.nowrap', {style : {opacity : dim ? '0.6' : '1'}}, name),
                   h('div.body.nowrap', detail)));
    }

    function actionRow(
        person: Person, detail: string, dot: string,
        actions: {label: string; style : 'primary' | 'chrome'; action : string; done : string; tip?: string}[]) {
        const row = personRow(person.username, person.avatarUrl, detail, dot);
        const buttons = h('div.actions');
        for (const a of actions) {
            const b = (a.style === 'chrome' ? h('button.chrome-button', {type : 'button'}, a.label)
                                            : h('button.btn.primary', {type : 'button'}, a.label)) as HTMLButtonElement;
            if (a.tip) tooltip(b, a.tip);
            b.onclick = () => friendAction(b, a.action, person.username, a.done);
            buttons.append(b);
        }
        row.append(buttons);
        return row;
    }

    function renderSocial() {
        const filter = searchInput.value.trim();
        const match = (name: string) => filter.length === 0 || name.toLowerCase().includes(filter.toLowerCase());
        const byName = new Map<string, PresencePlayer>();
        for (const p of presence?.players ?? [])
            if (!byName.has(p.displayName.toLowerCase())) byName.set(p.displayName.toLowerCase(), p);
        socialList.replaceChildren();

        const accepted = (friends?.friends ?? []).map(f => f.person).filter((p): p is Person => !!p);
        const online = accepted.filter(p => p.online);
        featureOnline.textContent = online.length > 0
                                        ? `${online.length} friend${online.length === 1 ? '' : 's'} online`
                                    : presence ? `${presence.onlinePlayers} online`
                                               : '';
        featureFaces.replaceChildren(...online.slice(0, 3).map((p, i) => {
            const f = avatar(28, p.username, p.avatarUrl);
            if (i > 0) f.style.marginLeft = '-8px';
            return f;
        }));

        if (onlineTab.classList.contains('checked')) {
            const players = (presence?.players ?? []).filter(p => match(p.displayName));
            section('Online now', presence?.onlinePlayers ?? players.length);
            for (const p of players) socialList.append(personRow(p.displayName, p.avatarUrl, activity(p), colors.good));
            if (!presence)
                socialList.append(note('Presence is unavailable right now.'));
            else if (players.length === 0)
                socialList.append(
                    note(filter.length > 0 ? 'No one matches that name.' : 'No one is online right now.'));
            return;
        }

        if (!friends) {
            socialList.append(note(friendsNote ?? 'Loading friends...'));
            return;
        }
        const incoming =
            friends.requests.filter(r => r.direction === 'Incoming' && r.person && match(r.person.username));
        const outgoing =
            friends.requests.filter(r => r.direction === 'Outgoing' && r.person && match(r.person.username));
        if (incoming.length) {
            section('Requests', incoming.length);
            for (const r of incoming)
                socialList.append(actionRow(r.person!, 'Wants to be friends', colors.accent, [
                    {
                        label : 'Accept',
                        style : 'primary',
                        action : 'accept',
                        done : `You and ${r.person!.username} are now friends.`
                    },
                    {
                        label : '',
                        style : 'chrome',
                        action : 'remove',
                        done : `Declined ${r.person!.username}'s request.`,
                        tip : 'Decline'
                    }
                ]));
        }
        if (outgoing.length) {
            section('Sent', outgoing.length);
            for (const r of outgoing)
                socialList.append(actionRow(r.person!, 'Request sent', colors.dim, [ {
                                                label : '',
                                                style : 'chrome',
                                                action : 'remove',
                                                done : `Cancelled your request to ${r.person!.username}.`,
                                                tip : 'Cancel request'
                                            } ]));
        }
        const byUser = (a: Person, b: Person) => a.username < b.username ? -1 : a.username > b.username ? 1 : 0;
        const shownOnline = online.filter(p => match(p.username)).sort(byUser);
        const shownOffline = accepted.filter(p => !p.online && match(p.username)).sort(byUser);
        if (shownOnline.length) {
            section('Online', shownOnline.length);
            for (const p of shownOnline) {
                const a = byName.get(p.username.toLowerCase());
                const row = personRow(p.username, p.avatarUrl, a ? activity(a) : 'Online', colors.good);
                messages.decorate(row, p.username);
                socialList.append(row);
            }
        }
        if (shownOffline.length) {
            section('Offline', shownOffline.length);
            for (const p of shownOffline) {
                const row = personRow(p.username, p.avatarUrl, 'Offline', colors.dim, true);
                messages.decorate(row, p.username);
                socialList.append(row);
            }
        }
        if (filter.length >= 2) {
            renderSearch(filter, accepted);
            return;
        }
        if (accepted.length === 0 && incoming.length === 0 && outgoing.length === 0)
            socialList.append(note('No friends yet. Search for a player above to send a request.'));
        else if (shownOnline.length + shownOffline.length + incoming.length + outgoing.length === 0)
            socialList.append(note('No one matches that name.'));
    }

    function renderSearch(filter: string, accepted: Person[]) {
        const friendNames = new Set(accepted.map(p => p.username.toLowerCase()));
        const pending =
            new Set((friends?.requests ?? []).filter(r => r.person).map(r => r.person!.username.toLowerCase()));
        const others =
            searchedFor === filter
                ? (searchResults ??
                   []).filter(p => !friendNames.has(p.username.toLowerCase()) && !pending.has(p.username.toLowerCase()))
                : [];
        section('Players', others.length);
        if (searchNote !== null) {
            socialList.append(note(searchNote));
            return;
        }
        if (searchedFor !== filter) {
            socialList.append(note('Searching...'));
            return;
        }
        for (const p of others)
            socialList.append(actionRow(p, p.online ? 'Online' : 'Offline', p.online ? colors.good : colors.dim, [
                {label : 'Add', style : 'primary', action : 'request', done : `Friend request sent to ${p.username}.`}
            ]));
        if (others.length === 0) socialList.append(note('No other players match that name.'));
    }

    async function stop(account: Account) {
        if (!stopArmed) {
            stopArmed = true;
            clearTimeout(disarmTimer);
            disarmTimer = window.setTimeout(() => {
                stopArmed = false;
                if (!shell.busy) refreshAccounts();
            }, 4000);
            await refreshAccounts();
            return;
        }
        stopArmed = false;
        clearTimeout(disarmTimer);
        shell.busy = true;
        setPlay('', 'STOPPING', false, `Closing ${account.displayName}'s game...`, '');
        try {
            await rpc('game.stop', {id : account.playerId});
            shell.showToast(`${account.displayName}'s game was closed.`);
        } catch (e) {
            shell.showToast("Couldn't close the game: " + messageOf(e));
        } finally {
            shell.busy = false;
            await refreshAccounts();
        }
    }

    async function play() {
        if (shell.busy) return;
        snapshot = await rpc<Snapshot>('session.snapshot');
        const account = selectedAccount();
        if (!account) return;
        const id = account.playerId;
        if (account.inGame) {
            await stop(account);
            return;
        }
        if (snapshot.needsGameSetup) {
            shell.showGameSetup(false);
            return;
        }
        if (!account.canSignInSilently) {
            shell.showLogin(false, id);
            return;
        }
        if (await shell.checkForUpdate()) {
            shell.showUpdatePrompt();
            return;
        }

        shell.busy = true;
        switcher.disabled = true;
        accountList.style.pointerEvents = 'none';
        setPlay('', 'LAUNCHING', false, 'Checking your account', '');
        show(launchProgress.el, true);
        let needsSignIn = false;
        const offStep = on('play.step', s => {
            launchTitle.textContent = s.title;
            launchDetail.textContent = s.detail;
        });
        const offProfile = on('play.profile', (p: Profile) => showProfile(p));
        try {
            const result = await rpc<{displayName : string}>('game.play', {id});
            shell.showToast(`${result.displayName} connected.`);
        } catch (e) {
            if (kindOf(e) === 'loginRequired')
                needsSignIn = true;
            else if (isCanceled(e))
                shell.showToast('The launch was cancelled or timed out. Try again.');
            else if (messageOf(e) === REPAIR_MESSAGE)
                shell.showToast(REPAIR_MESSAGE, {label : 'Repair', run : () => shell.showGameSetup(true, true)});
            else if (messageOf(e) === SMART_APP_CONTROL_MESSAGE)
                shell.showToast(SMART_APP_CONTROL_MESSAGE,
                                {label : 'Open settings', run : () => { void rpc('shell.openAppControl'); }});
            else
                shell.showToast(messageOf(e));
        } finally {
            offStep();
            offProfile();
            shell.busy = false;
            show(launchProgress.el, false);
            switcher.disabled = false;
            accountList.style.pointerEvents = '';
            if (!disposed) await refreshAccounts();
        }
        if (needsSignIn) shell.showLogin(false, id);
    }
    playButton.onclick = () => play();

    async function updateOutbreak(steam?: SteamSignIn) {
        if (outbreakCall) return;
        outbreakPercent = null;
        const offProgress = on('outbreak.progress', p => {
            if (p.fraction != null && /^Downloading/.test(p.stage)) outbreakPercent = Math.floor(p.fraction * 100);
            if (steamDialog.isOpen && /^(Downloading|Checking|Converting)/.test(p.stage)) steamDialog.started();
            renderOutbreakLine();
        });
        const offAsk = steam ? [on('steam.prompt', p => steamDialog.ask(p.kind, p.email, p.retry)), on('steam.qr', q => steamDialog.qr(q.rows))] : [];
        let askSteam = false;
        const call = rpc<{result : string}>('outbreak.install', steam ? {steam : true, ...steam} : {});
        outbreakCall = call;
        renderOutbreakLine();
        try {
            const result = await call;
            if (result.result === 'steam')
                askSteam = !!steam;
            else {
                steamDialog.close();
                shell.showToast('Outbreak is ready.');
            }
        } catch (e) {
            if (isCanceled(e))
                steamDialog.close();
            else if (steamDialog.isOpen)
                steamDialog.error(messageOf(e));
            else if (steam)
                shell.showToast(messageOf(e));
        } finally {
            offProgress();
            offAsk.forEach(off => off());
            outbreakCall = null;
            outbreakPercent = null;
            if (!disposed) await refreshAccounts();
        }
        if (askSteam && !disposed) steamDialog.open();
    }
    outbreakGet.onclick = () => {
        if (!outbreakCall) steamDialog.open();
    };

    function openGear() {
        sizeValue.textContent = SIZE_INFO[parseSize(snapshot?.prefs.downloadSize)].name;
        showMouse(snapshot?.prefs.mouseMultiplier ?? null);
        showFps(snapshot?.prefs.fpsLimit ?? null);
        show(gearMenu, true);
        gearMenu.animate([ {opacity : 0}, {opacity : 1} ], {duration : 150});
        (sizeItem as HTMLElement).focus();
    }
    function closeGear() {
        if (document.activeElement === mouse.input) mouse.input.blur();
        show(gearMenu, false);
    }
    gearButton.onclick = () => openGear();
    gearClose.onclick = () => closeGear();
    gearMenu.addEventListener('mousedown', e => {
        if (e.target === gearMenu) closeGear();
    });
    const onEscape = (e: KeyboardEvent) => {
        if (e.key !== 'Escape') return;
        if (sizeOverlay) {
            sizeOverlay.close();
            return;
        }
        if (!gearMenu.classList.contains('hidden')) closeGear();
        if (serversOpen()) openServers(false);
    };
    document.addEventListener('keydown', onEscape);
    let sizeOverlay: {close(): void}|null = null;
    function openSize() {
        const view = sizeView({
            embedded : true,
            onClose : () => {
                view.el.remove();
                view.dispose?.();
                sizeOverlay = null;
                if (!disposed) refreshAccounts();
            },
        });
        sizeOverlay = {close : () => view.close()};
        el.append(view.el);
        return view;
    }
    const notWhileBusy = (run: () => void) => () => {
        closeGear();
        if (shell.busy || selectedAccount()?.inGame) {
            shell.showToast(shell.busy ? 'Wait for the launch to finish first.' : 'Close the game first.');
            return;
        }
        run();
    };
    sizeItem.onclick = notWhileBusy(() => { openSize(); });
    verifyItem.onclick = verifyGame.onclick = notWhileBusy(() => shell.showGameSetup(true, true));
    changeGameItem.onclick = notWhileBusy(() => shell.showGameSetup(true));
    openGameItem.onclick = () => {
        closeGear();
        rpc('shell.openFolder', {which : 'game'});
    };
    tooltip(openGameItem, () => snapshot?.prefs.gamePath || null);

    const offExited =
        on('game.exited', async (g: {displayName: string; released : boolean; exitCode?: string | null}) => {
            if (!shell.busy)
                shell.showToast(g.exitCode ? `${g.displayName}'s game closed (code ${g.exitCode}).`
                                           : `${g.displayName}'s game closed.`);
            if (!shell.busy && !disposed) await refreshAccounts();
        });

    async function loadSettings() {
        const snap = snapshot ?? await rpc<Snapshot>('session.snapshot');
        const installed = shell.info.installed;
        instancesText.textContent = snap.instances;
        friendToggle.checked = snap.prefs.friendNotifications;
        messageToggle.checked = snap.prefs.messageNotifications ?? true;
        trayToggle.checked = snap.prefs.keepInTray;
        updateToggle.checked = snap.prefs.updateNotifications;
        startupToggle.disabled = !installed;
        startupText.textContent = installed ? 'Open RWND quietly in the tray when you sign in to Windows.'
                                            : 'Available in the installed launcher.';
        versionText.textContent = 'RWND Parabellum v' + shell.info.version;
        updateText.textContent = installed
                                     ? 'Updates install when RWND starts, and RWND checks every 10 minutes while open.'
                                     : 'Development build: automatic updates are off.';
        updateButton.disabled = !installed;
        installText.textContent = shell.info.installDirectory;
        show(uninstallRow, installed);
        startupToggle.checked = false;
        if (installed) rpc<{enabled : boolean}>('startup.get').then(s => { startupToggle.checked = s.enabled; });
    }

    function showMouse(value: number|null) {
        mouse.input.value = value === null ? '0.02' : String(Number(value.toFixed(6)));
        mouseHint.textContent = 'Default 0.02';
        mouseHint.style.color = '';
        mouse.box.classList.remove('invalid');
    }
    const mouseValid = (text: string) =>
        /^\d*\.?\d+$/.test(text) && !/\.\d{7,}$/.test(text) && Number(text) >= 0.001 && Number(text) <= 0.1;
    async function saveMouse(text: string) {
        text = text.trim();
        if (!mouseValid(text)) {
            mouseHint.textContent = 'Use 0.001 to 0.1';
            mouseHint.style.color = 'var(--bad)';
            mouse.box.classList.add('invalid');
            return;
        }
        try {
            snapshot = await rpc<Snapshot>('prefs.update', {mouseMultiplier : text});
            showMouse(snapshot.prefs.mouseMultiplier ?? null);
        } catch (e) {
            mouseHint.textContent = messageOf(e);
            mouseHint.style.color = 'var(--bad)';
        }
    }
    mouse.input.addEventListener('change', () => saveMouse(mouse.input.value));
    mouse.input.addEventListener('keydown', e => {
        if (e.key === 'Enter') mouse.input.blur();
    });
    mouseReset.onclick = () => {
        mouse.input.value = '0.02';
        saveMouse('0.02');
    };

    function showFps(value: number|null) {
        const on = value !== null && value > 0;
        fpsToggle.checked = on;
        show(fps.box, on);
        if (on) fps.input.value = String(value);
        fpsHint.textContent = on ? '30 to 1000' : 'Off';
        fpsHint.style.color = '';
        fps.box.classList.remove('invalid');
    }
    async function saveFps(value: string) {
        try {
            snapshot = await rpc<Snapshot>('prefs.update', {fpsLimit : value});
            showFps(snapshot.prefs.fpsLimit ?? null);
        } catch (e) {
            fpsHint.textContent = messageOf(e);
            fpsHint.style.color = 'var(--bad)';
        }
    }
    const fpsValid = (text: string) => /^\d{2,4}$/.test(text) && Number(text) >= 30 && Number(text) <= 1000;
    function setFps(on: boolean) {
        if (!on) {
            saveFps('0');
            return;
        }
        const text = fps.input.value.trim();
        saveFps(fpsValid(text) ? text : '144');
    }
    fps.input.addEventListener('change', () => {
        const text = fps.input.value.trim();
        if (!fpsValid(text)) {
            fpsHint.textContent = 'Use 30 to 1000';
            fpsHint.style.color = 'var(--bad)';
            fps.box.classList.add('invalid');
            return;
        }
        saveFps(text);
    });
    fps.input.addEventListener('keydown', e => {
        if (e.key === 'Enter') fps.input.blur();
    });

    async function saveToggles() {
        snapshot = await rpc<Snapshot>('prefs.update', {
            friendNotifications : friendToggle.checked,
            messageNotifications : messageToggle.checked,
            keepInTray : trayToggle.checked,
            updateNotifications : updateToggle.checked
        });
    }

    async function setStartup(enabled: boolean) {
        try {
            startupToggle.checked = (await rpc<{enabled : boolean}>('startup.set', {enabled})).enabled;
        } catch (e) {
            startupToggle.checked = (await rpc<{enabled : boolean}>('startup.get')).enabled;
            shell.showToast(e instanceof RpcError && e.kind === 'unauthorizedAccess'
                                ? "Windows didn't allow changing startup apps."
                                : messageOf(e));
        }
    }

    openInstances.onclick = () => rpc('shell.openFolder', {which : 'instances'});
    openInstall.onclick = () => rpc('shell.openFolder', {which : 'install'});
    updateButton.onclick = async () => {
        updateButton.disabled = true;
        updateText.textContent = 'Checking...';
        try {
            const release = await rpc<Release|null>('update.check');
            if (!release) {
                updateText.textContent = "You're on the latest version.";
                return;
            }
            updateText.textContent = `v${release.version} is available.`;
            shell.offerUpdate(release, false);
        } catch (e) {
            if (kindOf(e) !== 'canceled') updateText.textContent = "Couldn't check for updates: " + messageOf(e);
        } finally {
            updateButton.disabled = false;
        }
    };
    uninstall.onclick = () => {
        if (!shell.busy) rpc('uninstall.start');
    };

    const hooks = {
        playFromTray : () => {
            const a = selectedAccount();
            if (!playButton.disabled && a && !a.inGame) play();
        },
        signOutFromTray : () => signOut(),
        showFriends : () => {
            selectTab(false);
            setSocialTab(false);
            searchInput.value = '';
            renderSocial();
            queueSearch();
        },
        openSettings : () => {
            checkNav('settings');
            showPage(settingsPage);
        },
        showMessages : (name?: string) => openMessages(name),
    };
    shell.home = hooks;
    const runningTimer = window.setInterval(() => {
        if (!shell.busy) refreshAccounts();
    }, 2000);
    const socialTimer = window.setInterval(() => {
        loadSocial();
        loadServers();
    }, 30_000);

    selectTab(false);
    setSocialTab(false);
    const ready = (async () => {
        await refreshAccounts();
        if (openSettingsFirst) hooks.openSettings();
        await Promise.all([ loadNews(), loadProfile(), loadSocial(), loadServers() ]);
    })();

    return {
        el,
        ready,
        dispose() {
            disposed = true;
            document.removeEventListener('keydown', onEscape);
            document.removeEventListener('mousedown', onServersOutside);
            clearInterval(runningTimer);
            clearInterval(socialTimer);
            clearTimeout(searchTimer);
            clearTimeout(disarmTimer);
            offExited();
            media.dispose();
            ambient.dispose();
            messages.dispose();
            if (shell.home === hooks) shell.home = undefined;
        },
        showSample(profile, news, sampleFriends, samplePresence) {
            showProfile(profile);
            newsList.replaceChildren(...news.map(newsCard));
            friends = sampleFriends;
            presence = samplePresence;
            friendsNote = null;
            renderSocial();
        },
        async openAccountMenu() {
            await refreshAccounts();
            openMenu();
        },
        previewOutbreakSteam() { steamDialog.previewSteam(); },
        previewGear(hover) { gearButton.classList.toggle('hover', hover); },
        previewGearMenu() { openGear(); },
        previewServersOpen() { openServers(true); },
        previewVote(title, question) { void voteDialog.ask({title, text : question || 'Pick the next season.'}); },
        previewLaunchProgress(title, detail) {
            shell.busy = true;
            launchTitle.textContent = title;
            show(launchProgress.el, true);
            launchDetail.textContent = detail;
        },
        previewSize() { return openSize(); },
        previewMessages(state, open, history, draft) {
            messages.showSample(state, open, history, draft);
            renderSocial();
            if (open) openMessages();
        },
        previewOutbreakProgress() {
            shell.busy = true;
            outbreakCall = {cancel() {}};
            outbreakPercent = 41;
            shell.busy = false;
            renderOutbreakLine();
        },
        openSettings() {
            checkNav('settings');
            showPage(settingsPage);
        },
    };
}
