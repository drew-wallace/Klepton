"""Eleven Table Tennis Steam Android target."""
TARGETS = {
    'eleven': {
        'libs': None,
        'steam_local': True,
        'srcdir': 'eleven/lib/arm64-v8a',
        'tree': 'eleven',
        'apk': 'eleven.apk',
        'assets': 'eleven/assets',
        'qtplugins': '',
        'obb': 'obb',
        'entry': 'libmain',
        'kind': 'unity',
        'product': 'KleptonEleven',
        'display': 'Eleven Table Tennis',
    },
}
SOURCES = {'eleven': 'eleven/eleven.apk'}
