import {rpc} from './bridge';
import {setMedia} from './backdrop';
import {startCrashReporter} from './crashReporter';
import {shell, type AppInfo} from './shell';
import {startPreview} from './preview';
import {firstRunView} from './views/firstRun';
import {gameSetupView} from './views/gameSetup';
import {homeView} from './views/home';
import {showLegalNotice} from './views/legal';
import {loginView} from './views/login';
import {setupView} from './views/setup';
import {sizeView} from './views/sizePage';

async function main() {
    const info = await rpc<AppInfo>('app.info');
    setMedia(info.media);
    shell.init(info, {
        login : (adding, reauthenticate) => loginView(adding, reauthenticate),
        home : settings => homeView(settings),
        gameSetup : (fromSettings, verify) => gameSetupView(fromSettings, verify),
        firstRun : () => firstRunView(),
        size : () => sizeView(),
        setup : () => setupView(),
    });
    await document.fonts.load('500 16px Rajdhani');
    await Promise.all(
        [ '600 16px Rajdhani', '700 16px Rajdhani', '60px "Saira Stencil One"' ].map(f => document.fonts.load(f)));
    if (info.mode === 'preview')
        startPreview();
    else {
        if (info.mode === 'launcher') {
            await showLegalNotice();
            startCrashReporter(text => shell.showToast(text));
        }
        await shell.start();
    }
}

main();
