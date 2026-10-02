"""Local APK target profiles for walkabout."""

TARGETS = {}

TARGETS['walkabout-57013'] = {
    'libs': None,
    'guest_prepare': 'games/walkabout/prepare_guest.sh',
    'steam_local': True,
    'srcdir': 'walkabout-57013/lib/arm64-v8a',
    'tree': 'walkabout-57013',
    'apk': 'walkabout-57013.apk',
    'assets': 'walkabout-57013/assets',
    'qtplugins': '',
    'obb': 'obb',
    # This folder also carries 124 loose Addressables bundles; stage it whole.
    'obb_raw': '1',
    'entry': 'libmain',
    'kind': 'unity',
    'product': 'KleptonWalkabout57013',
    'display': 'Walkabout Mini Golf 6.7',
}

SOURCES = {'walkabout-57013': 'Walkabout Mini Golf - Steam/WalkaboutMiniGolf.apk'}
