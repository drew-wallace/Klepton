"""Local APK target profiles for 4xvr."""

TARGETS = {}

TARGETS['4xvr-11026-vrp'] = {
    'libs': None,
    'srcdir': '4xvr-11026-vrp/lib/arm64-v8a',
    'tree': '4xvr-11026-vrp',
    'apk': '4xvr-11026-vrp.apk',
    'assets': '4xvr-11026-vrp/assets',
    'qtplugins': '',
    'obb': 'obb',
    'entry': 'libvr4p-oculus',
    'kind': 'native',
    'product': 'Klepton4Xvr11026Vrp',
    'display': '4XVR 1.10.26 VRP',
}

TARGETS['4xvr-11026'] = {
    'libs': None,
    'srcdir': '4xvr-11026/lib/arm64-v8a',
    'tree': '4xvr-11026',
    'apk': '4xvr-11026.apk',
    'assets': '4xvr-11026/assets',
    'qtplugins': '',
    'obb': 'obb',
    'entry': 'libvr4p-oculus',
    'kind': 'native',
    'product': 'Klepton4Xvr11026',
    'display': '4XVR 1.10.26',
}

SOURCES = {'4xvr-11026': '4xvr/4xvr.apk', '4xvr-11026-vrp': '4xvr/4XVR Video Player (Pro + Trial Bypass) v11026+1.10.26 -VRP/cn.vr4p.oculus4xvrplayerov.apk'}
