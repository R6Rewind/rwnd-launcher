import {on, rpc} from './bridge';
import {show, sleep} from './dom';
import {shell} from './shell';
import {firstRunView} from './views/firstRun';
import {gameSetupView} from './views/gameSetup';
import {homeView, type FriendLink, type PresencePlayer} from './views/home';
import {legalDialog} from './views/legal';
import {loginView} from './views/login';
import {setupView} from './views/setup';

const friend = (username: string, online: boolean): FriendLink =>
    ({person : {username, avatarUrl : null, online}, direction : 'Outgoing'});
const online = (displayName: string, activity: string, match: PresencePlayer['match']): PresencePlayer =>
    ({displayName, avatarUrl : null, activity, match});
const nowDate = () => {
    const d = new Date();
    return `${String(d.getDate()).padStart(2, '0')} ${
            ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'][d.getMonth()]} ${
        d.getFullYear()}`;
};

function sample(home: ReturnType<typeof homeView>) {
    home.showSample(
        {
            id : '10000000-0000-4000-8000-000000000001',
            displayName : 'Player1',
            avatarUrl : null,
            role : 'Player',
            rating : {season : 'S1', rating : 2614, deviation : 80, wins : 37, losses : 21, rank : 'Gold II'},
            progression : {level : 42, experience : 0},
            wallet : {renown : 18250, credits : 600}
        },
        [
            {
                id : '00000000-0000-4000-8000-000000000101',
                title : 'Parabellum servers are live',
                body : 'Quick Play and Ranked queues are open on RWND.',
                category : 'Announcement',
                banner : true,
                severity : 'Info',
                publishAt : '',
                publishDate : nowDate(),
                publishTicks : 2
            },
            {
                id : '00000000-0000-4000-8000-000000000102',
                title : 'Launcher 1.0',
                body : 'New launcher with remembered logins and automatic updates.',
                category : 'Update',
                banner : false,
                severity : 'Info',
                publishAt : '',
                publishDate : nowDate(),
                publishTicks : 1
            }
        ],
        {
            friends : [
                friend('AquaNova', true), friend('IamMorrison', true), friend('CrimsonWhisper', true),
                friend('StellarEcho', false), friend('NebulaKnight', false)
            ],
            requests : [ {person : {username : 'Player3', avatarUrl : null, online : true}, direction : 'Incoming'} ]
        },
        {
            onlinePlayers : 14,
            players : [
                online(
                    'AquaNova', 'InMatch',
                    {map : 'Villa', playlist : 'Ranked', round : 4, totalRounds : 7, score : 0, kills : 3, deaths : 1}),
                online('IamMorrison', 'Matchmaking', null), online('CrimsonWhisper', 'MainMenu', null),
                online(
                    'Kapkan_Kid', 'InMatch',
                    {map : 'Bank', playlist : 'Casual', round : 2, totalRounds : 6, score : 0, kills : 1, deaths : 2})
            ]
        });
}

const OUTBREAK_SAMPLE = {
    live : true,
    ready : false,
    update : false,
    steam : false,
    installing : false
};
const servers = (...allowed: string[]) => ({regions : [ {id : 'na', name : 'NA'}, {id : 'eu', name : 'EU'} ], allowed});
const READY_SAMPLE = {
    live : true,
    ready : true,
    update : false,
    steam : true,
    installing : false
};

async function renderFirstRun(screen: string): Promise<boolean> {
    const folder = String.raw`D:\Games\RWND\ParaBellum`;
    const missing = {ok : false, title : 'Folder not found', reason : '', folder : null, kind : 'missing' as const};
    const homeWith =
        async (outbreak: object, size: 'full'|'medium'|'small' = 'full', mouseMultiplier: number|null = null,
               fpsLimit: number|null = null, serverChoice: object|null = null) => {
        await rpc('preview.session', {
            accounts : 1,
            gamePath : '',
            outbreak,
            downloadSize : size,
            mouseMultiplier,
            fpsLimit,
            servers : serverChoice
        });
        const home = homeView(false);
        shell.show(home);
        await home.ready;
        await sleep(300);
        sample(home);
        return home;
    };
    const firstRun = async () => {
        await rpc('preview.session', {accounts : 1, gamePath : '-', outbreak : OUTBREAK_SAMPLE});
        const view = firstRunView();
        shell.show(view);
        await view.ready;
        return view;
    };
    const sizePageOf = async (current: 'full'|'medium'|'small') => {
        const view = (await homeWith(READY_SAMPLE, current)).previewSize();
        await view.ready;
        return view;
    };
    switch (screen) {
    case '00-installer-unchanged':
        shell.show(setupView());
        return true;
    case '01-sign-in':
        await rpc('preview.session', {accounts : 0, gamePath : '-', steam : true});
        shell.show(loginView(false));
        return true;
    case '02-game-folder-not-installed':
        (await firstRun()).previewCheck(missing, folder);
        return true;
    case '03-game-folder-found':
        (await firstRun())
            .previewCheck({
                ok : true,
                title : 'Para Bellum (Y3S2) found',
                reason : '',
                folder : String.raw`E:\ParaBellum`,
                kind : 'ok'
            },
                          String.raw`E:\ParaBellum`);
        return true;
    case '04-game-folder-other-siege-version':
        (await firstRun())
            .previewCheck({
                ok : false,
                title : 'Wrong Siege version',
                reason : "This RainbowSix.exe isn't the Para Bellum (Y3S2) build.",
                folder : null,
                kind : 'other'
            },
                          String.raw`C:\Program Files (x86)\Steam\steamapps\common\Tom Clancy's Rainbow Six Siege`);
        return true;
    case '02b-download-stopped-error':
        (await firstRun())
            .previewCheck({
                ...missing,
                kind : 'partial',
                stopTitle : 'Download stopped',
                stopReason : "Steam didn't accept the Steam Guard code. Try again with a new code."
            },
                          folder);
        return true;
    case '02c-download-stopped-by-you':
        (await firstRun())
            .previewCheck({
                ...missing,
                kind : 'partial',
                stopTitle : 'Download stopped',
                stopReason : 'You stopped it. Picks up where it stopped.'
            },
                          folder);
        return true;
    case '05-download-size': {
        const v = await firstRun();
        v.previewCheck(missing, folder);
        v.previewSize();
        return true;
    }
    case '06-download-size-steam-password': {
        const v = await firstRun();
        v.previewCheck(missing, folder);
        v.previewSize('password');
        return true;
    }
    case '05b-download-size-steam-sign-in': {
        const v = await firstRun();
        v.previewCheck(missing, folder);
        v.previewSize('choice');
        return true;
    }
    case '07b-download-steam-guard-code':
        (await firstRun()).previewPrompt('appCode');
        return true;
    case '07-download-outbreak-first':
        (await firstRun()).previewProgress('active', {
            stage : 'Downloading Outbreak',
            part : 0,
            parts : 0,
            fraction : 0.62,
            detail : 'datapc64_merged_fury_bnk_textures3.forge',
            job : 'outbreak'
        });
        return true;
    case '08-download-second-steam-sign-in':
        (await firstRun()).previewQr();
        return true;
    case '09-download-game':
        (await firstRun())
            .previewProgress(
                'done', {stage : 'Downloading', part : 1, parts : 2, fraction : 0.4172, detail : '14.2 GB downloaded'});
        return true;
    case '10-home-play-gear':
        await homeWith(READY_SAMPLE, 'medium');
        return true;
    case '11-home-play-gear-hover':
        (await homeWith(READY_SAMPLE, 'medium')).previewGear(true);
        return true;
    case '12-gear-menu':
        (await homeWith(READY_SAMPLE, 'medium')).previewGearMenu();
        return true;
    case '13-gear-download-size-page':
        (await sizePageOf('medium')).previewPick('small');
        return true;
    case '14-confirm-medium-to-small':
        (await sizePageOf('medium')).previewConfirm('small');
        return true;
    case '15-small-to-full-steam-sign-in':
        (await sizePageOf('small')).previewSteam('full');
        return true;
    case '16-small-to-full-progress':
        (await sizePageOf('small')).previewProgress('full');
        return true;
    case '17-home-outbreak-background':
        (await homeWith({...OUTBREAK_SAMPLE, steam : true}, 'medium')).previewOutbreakProgress();
        return true;
    case '18-home-outbreak-get':
        await homeWith(OUTBREAK_SAMPLE, 'medium');
        return true;
    case '19-outbreak-steam-sign-in':
        (await homeWith(OUTBREAK_SAMPLE, 'medium')).previewOutbreakSteam();
        return true;
    case '23-gear-fps-limit-on':
        (await homeWith(READY_SAMPLE, 'medium', 0.00223, 144)).previewGearMenu();
        return true;
    case '23-gear-fps-limit-off':
        (await homeWith(READY_SAMPLE, 'medium', 0.00223, null)).previewGearMenu();
        return true;
    case '22-gear-mouse-multiplier':
        (await homeWith(READY_SAMPLE, 'medium', 0.00223)).previewGearMenu();
        return true;
    case '20-settings': {
        const home = await homeWith(READY_SAMPLE, 'medium');
        home.openSettings();
        return true;
    }
    case '25-servers-global':
        await homeWith(READY_SAMPLE, 'medium', null, null, servers('na', 'eu'));
        return true;
    case '26-servers-open':
        (await homeWith(READY_SAMPLE, 'medium', null, null, servers('na', 'eu'))).previewServersOpen();
        return true;
    case '29-home-season-vote':
        (await homeWith(READY_SAMPLE, 'medium'))
            .previewVote('Next season: Round 1 of 2', 'Which season should RWND play next?');
        return true;
    case '27-servers-eu-only':
        await homeWith(READY_SAMPLE, 'medium', null, null, servers('eu'));
        return true;
    case '28-servers-launch-progress':
        (await homeWith(READY_SAMPLE, 'medium', null, null, servers('na')))
            .previewLaunchProgress('Installing startup files', 'Checking verified RWND components');
        return true;
    }
    return false;
}
const sampleProfile = {
    id : '10000000-0000-4000-8000-000000000001',
    displayName : 'Player1',
    avatarUrl : null,
    role : 'Player',
    rating : {season : 'S1', rating : 2614, deviation : 80, wins : 37, losses : 21, rank : 'Gold II'},
    progression : {level : 42, experience : 0},
    wallet : {renown : 18250, credits : 600}
};
const sampleFriends = {
    friends : [
        friend('AquaNova', true), friend('IamMorrison', true), friend('CrimsonWhisper', true),
        friend('StellarEcho', false), friend('NebulaKnight', false)
    ],
    requests : [] as FriendLink[]
};
const samplePresence = {
    onlinePlayers : 14,
    players : [
        online('AquaNova', 'InMatch',
               {map : 'Villa', playlist : 'Ranked', round : 4, totalRounds : 7, score : 0, kills : 3, deaths : 1}),
        online('IamMorrison', 'Matchmaking', null),
        online('CrimsonWhisper', 'InMatch',
               {map : 'Bank', playlist : 'Casual', round : 2, totalRounds : 6, score : 0, kills : 1, deaths : 2})
    ]
};

async function render(screen: string) {
    show(document.getElementById('overlay')!, false);
    show(document.getElementById('toast')!, false);
    const session = (accounts: number, gamePath?: string) =>
        rpc('preview.session', {accounts, gamePath : gamePath ?? ''});
    if (!(await renderFirstRun(screen))) switch (/^4[a-z]-home$/.test(screen) ? '4-home' : screen) {
        case '1-setup':
            shell.show(setupView());
            break;
        case '1b-game-folder':
            await session(0, '-');
            shell.show(gameSetupView(false));
            break;
        case '1c-game-folder-wrong':
            await session(0, 'C:\\Games\\OldR6\\BurntHorizon');
            shell.show(gameSetupView(false));
            break;
        case '1d-steam':
        case '1d2-steam-password':
        case '1e-steam-guard':
        case '1f-steam-progress': {
            await session(0);
            const view = gameSetupView(false);
            shell.show(view);
            await view.ready;
            if (screen === '1d-steam')
                view.previewSteam();
            else if (screen === '1d2-steam-password')
                view.previewSteam(true);
            else if (screen === '1e-steam-guard')
                view.previewPrompt('appCode');
            else
                view.previewProgress({
                    stage : 'Downloading',
                    part : 2,
                    parts : 2,
                    fraction : 0.4172,
                    detail : 'datapc64_merged_bnk_textures3.forge'
                });
            break;
        }
        case '2-login':
            await session(0);
            shell.show(loginView(false));
            break;
        case '2b-login-steam':
            await rpc('preview.session', {accounts : 0, gamePath : '', steam : true});
            shell.show(loginView(false));
            break;
        case '3-login-known':
            await session(2);
            shell.show(loginView(false));
            break;
        case '4-home': {
            await session(screen === '4-home' ? 2 : 3);
            const home = homeView(false);
            shell.show(home);
            await sleep(400);
            sample(home);
            break;
        }
        case '9-messages':
        case '9b-home-unread': {
            await session(2);
            const home = homeView(false);
            shell.show(home);
            await sleep(400);
            home.showSample(sampleProfile, [], sampleFriends, samplePresence);
            const ago = (minutes: number) => new Date(Date.now() - minutes * 60_000).toISOString();
            const chat = (name: string, online: boolean, game: boolean, unread: number,
                          last: [ string, boolean, number ]|null) => ({
                name,
                avatarUrl : null,
                online,
                game,
                unread,
                last : last ? {text : last[0], mine : last[1], at : ago(last[2])} : null
            });
            const state = {
                available : true,
                connected : true,
                friends : [
                    chat('AquaNova', true, true, screen === '9-messages' ? 0 : 2,
                         [ 'ranked after this one?', false, 1 ]),
                    chat('IamMorrison', true, false, 0, [ 'on my way', true, 40 ]),
                    chat('CrimsonWhisper', true, true, 1, [ 'gg', false, 12 ]),
                    chat('StellarEcho', false, false, 0, [ 'thanks for the carry', false, 60 * 26 ]),
                    chat('NebulaKnight', false, false, 0, null)
                ]
            };
            const m = (id: number, mine: boolean, text: string, minutes: number) =>
                ({with : 'AquaNova', id, mine, text, at : ago(minutes)});
            const history = [
                m(1, false, 'you on tonight?', 31), m(2, true, 'yes, after dinner', 24),
                m(3, true, 'squad up at 8', 24), m(4, false, 'nice. Villa or Bank?', 2),
                m(5, false, 'ranked after this one?', 1)
            ];
            home.previewMessages(state, screen === '9-messages' ? 'AquaNova' : '',
                                 screen === '9-messages' ? history : [],
                                 screen === '9-messages' ? 'sure, invite me' : undefined);
            if (screen === '9b-home-unread') home.showSample(sampleProfile, [], sampleFriends, samplePresence);
            break;
        }
        case '5-home-menu': {
            await session(3);
            const home = homeView(false);
            shell.show(home);
            await sleep(300);
            await home.openAccountMenu();
            break;
        }
        case '7-update-available': {
            await session(1);
            shell.show(homeView(false));
            shell.previewUpdatePrompt({
                version : '1.0.5',
                url : 'https://example.invalid/',
                size : 1,
                notes : 'In-game overlay matches the new design'
            });
            break;
        }
        case '6-settings': {
            await session(1);
            const home = homeView(false);
            shell.show(home);
            await sleep(300);
            home.openSettings();
            break;
        }
        case '8-outbreak-update':
        case '8b-outbreak-steam':
        case '8c-outbreak-progress': {
            await rpc('preview.session', {
                accounts : 1,
                gamePath : '',
                outbreak : {live : true, ready : false, update : false, steam : false, installing : false}
            });
            const home = homeView(false);
            shell.show(home);
            await home.ready;
            await sleep(300);
            if (screen === '8b-outbreak-steam') home.previewOutbreakSteam();
            if (screen === '8c-outbreak-progress') home.previewOutbreakProgress();
            break;
        }
        case '23z-legal':
            await session(0);
            shell.show(loginView(false));
            document.getElementById('root')!.append(legalDialog(() => {}));
            break;
        case '24-cant-reach-rwnd':
            void shell.update();
            break;
        }
    await sleep(3000);
    await document.fonts.ready;
    rpc('preview.rendered', {screen});
}

export function startPreview() {
    on('preview.show', s => { render(s.screen); });
    rpc('preview.start');
}
