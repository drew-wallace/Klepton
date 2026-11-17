"""Verified Pistol Whip Steam Frame build and its runtime defaults."""

TARGETS = {
    'pistolwhip-12': {
        'libs': None,
        'srcdir': 'pistolwhip/lib/arm64-v8a',
        'tree': 'pistolwhip',
        'apk': 'pistolwhip.apk',
        'assets': 'pistolwhip/assets',
        'qtplugins': '',
        'obb': 'obb',
        'entry': 'libmain',
        'kind': 'unity',
        'steam_local': True,
        'steam_diagnostics': True,
        'product': 'KleptonPistolWhip',
        'display': 'Pistol Whip (Steam Frame 1.6.0.2)',
        # Keep the package-derived app and container paths used before this
        # profile existed, including saved data in existing installer imports.
        'installer_target': 'import-93242634a1ab5a06',
        # On-device scene loads peaked near 8 GiB at 3072 pixels. At 2048,
        # multiple scene loads stayed below 5.1 GiB without runtime faults.
        'runtime_defaults': {'KL_OVRP_EYE_MAX': '2048'},
        'compatibility_note': 'Uses a 2048-pixel eye render cap to keep memory headroom during scene loading.',
    },
}
