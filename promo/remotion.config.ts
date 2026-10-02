import {Config} from '@remotion/cli/config';

Config.setVideoImageFormat('jpeg');
Config.setJpegQuality(95);
Config.setCodec('h264');
Config.setCrf(16);
Config.setPixelFormat('yuv420p');
Config.setOverwriteOutput(true);
// Reuse the installed Chrome instead of downloading Remotion's headless shell.
Config.setBrowserExecutable(process.env.REMOTION_CHROME ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome');
