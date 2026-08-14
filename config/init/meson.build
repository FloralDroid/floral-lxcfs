# SPDX-License-Identifier: LGPL-2.1-or-later

if 'systemd' in init_script
    systemd = dependency('systemd')
    systemd_system_unit_dir = systemd.get_pkgconfig_variable('systemdsystemunitdir')
    systemd_service = custom_target(
        'floral-lxcfs.service',
        input: 'systemd/floral-lxcfs.service.in',
        output: 'floral-lxcfs.service',
        command: [
            meson_render_jinja2,
            config_h,
            '@INPUT@',
            '@OUTPUT@',
        ],
        install: true,
        install_dir: systemd_system_unit_dir)
endif

if 'upstart' in init_script
        error('The isolated Floral branch currently provides only floral-lxcfs.service')
endif

if 'openrc' in init_script
        error('The isolated Floral branch currently provides only floral-lxcfs.service')
endif

if 'sysvinit' in init_script
        error('The isolated Floral branch currently provides only floral-lxcfs.service')
endif
