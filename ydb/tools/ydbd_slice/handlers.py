import os
import time
import logging
import subprocess
from collections import deque, defaultdict


logger = logging.getLogger(__name__)


class CalledProcessError(subprocess.CalledProcessError):
    def __init__(self, base):
        super(CalledProcessError, self).__init__(base.returncode, base.cmd, base.output)

    def __str__(self):
        return "Command '%s' returned non-zero exit status %d and output was '%s'" % (
            self.cmd,
            self.returncode,
            self.output
        )


def format_drivers(nodes):
    cmd = r"sudo find /dev/disk/ -path '*/by-partlabel/kikimr_*' " \
          r"-exec dd if=/dev/zero of={} bs=1M count=1 status=none \;"
    nodes.execute_async(cmd)


def _ensure_berkanavt_exists(nodes):
    cmd = r"sudo mkdir -p /Berkanavt"
    nodes.execute_async(cmd)


def _clear_registered_slots(nodes):
    nodes.execute_async(r"sudo find /Berkanavt/ -maxdepth 1 -type d  -name 'kikimr_*' -exec  rm -rf -- {} \;")


def _clear_slot(nodes, slot):
    cmd = r"sudo find /Berkanavt/ -maxdepth 1 -type d  -name kikimr_{slot} -exec  rm -rf -- {{}} \;".format(slot=slot.slot)
    nodes.execute_async(cmd)


def _clear_logs(nodes):
    cmd = "sudo service rsyslog stop; " \
        "find /Berkanavt/ -mindepth 2 -maxdepth 2 -name logs | egrep '^/Berkanavt/kikimr' | sudo xargs -I% find % -mindepth 1 -delete; " \
        "sudo service rsyslog start;"
    nodes.execute_async(cmd)


def slice_format(components, nodes, cluster_details, walle_provider):
    slice_stop(components, nodes, cluster_details), walle_provider
    format_drivers(nodes)
    slice_start(components, nodes, cluster_details, walle_provider)


def slice_clear(components, nodes, cluster_details, walle_provider):
    slice_stop(components, nodes, cluster_details, walle_provider)

    if 'dynamic_slots' in components:
        for slot in cluster_details.dynamic_slots.values():
            _clear_slot(nodes, slot)

    if 'kikimr' in components:
        format_drivers(nodes)


def _invoke_scripts(dynamic_cfg_path, scripts):
    for script_name in scripts:
        script_path = os.path.join(dynamic_cfg_path, script_name)
        if os.path.isfile(script_path):
            cmd = ["bash", script_path]
            logger.info("run cmd '%s'", cmd)
            try:
                subprocess.check_output(cmd, stderr=subprocess.STDOUT)
            except subprocess.CalledProcessError as er:
                raise CalledProcessError(er)


def _dynamic_configure(configurations):
    dynamic_cfg_path = configurations.create_dynamic_cfg()
    # wait for bs to configure
    time_remaining = 60
    while True:
        try:
            _invoke_scripts(dynamic_cfg_path, ['init_storage.bash'])
            break
        except CalledProcessError:
            time_to_wait = min(time_remaining, 5)
            if not time_to_wait:
                raise
            time_remaining -= time_to_wait
            time.sleep(time_to_wait)
    _invoke_scripts(
        dynamic_cfg_path, (
            "init_cms.bash",
            "init_compute.bash",
            "init_root_storage.bash",
            "init_databases.bash"
        )
    )


def slice_install(components, nodes, cluster_details, configurator, do_clear_logs, args, walle_provider):
    _ensure_berkanavt_exists(nodes)
    slice_stop(components, nodes, cluster_details, walle_provider)

    if 'dynamic_slots' in components or 'kikimr' in components:
        _stop_all_slots(nodes)
        _clear_registered_slots(nodes)

    if do_clear_logs:
        _clear_logs(nodes)

    if 'kikimr' in components:
        format_drivers(nodes)

        if 'bin' in components.get('kikimr', []):
            _update_kikimr(nodes, configurator.kikimr_bin, configurator.kikimr_compressed_bin)

        if 'cfg' in components.get('kikimr', []):
            static_cfg_path = configurator.create_static_cfg()
            _update_cfg(nodes, static_cfg_path)
            _deploy_secrets(nodes, args.yav_version)

        _start_static(nodes)
        _dynamic_configure(configurator)

    _deploy_slot_configs(components, nodes, cluster_details, walle_provider)
    _start_dynamic(components, nodes, cluster_details, walle_provider)


def _get_available_slots(components, nodes, cluster_details, walle_provider):
    if 'dynamic_slots' not in components:
        return {}

    slots_per_domain = {}

    for domain in cluster_details.domains:
        available_slots_per_zone = defaultdict(deque)
        all_available_slots_count = 0

        for slot in cluster_details.dynamic_slots.values():
            if slot.domain == domain.domain_name:
                for node in nodes.nodes_list:
                    item = (slot, node)
                    available_slots_per_zone[walle_provider.get_datacenter(node).lower()].append(item)
                    available_slots_per_zone['any'].append(item)
                    all_available_slots_count += 1
        slots_per_domain[domain.domain_name] = available_slots_per_zone

    return (slots_per_domain, all_available_slots_count, )


def _deploy_slot_config_for_tenant(nodes, slot, tenant, node):
    slot_dir = "/Berkanavt/kikimr_{slot}".format(slot=slot.slot)
    logs_dir = slot_dir + "/logs"
    slot_cfg = slot_dir + "/slot_cfg"
    env_txt = slot_dir + "/env.txt"
    cfg = """\
tenant=/{domain}/{tenant}
grpc={grpc}
mbus={mbus}
ic={ic}
mon={mon}""".format(
        domain=slot.domain,
        tenant=tenant.name,
        mbus=slot.mbus,
        grpc=slot.grpc,
        mon=slot.mon,
        ic=slot.ic,
    )

    escaped_cmd = cfg.encode('unicode_escape').decode()

    cmd = "sudo sh -c 'mkdir -p {logs_dir}; sudo chown syslog {logs_dir}; touch {env_txt}; /bin/echo -e \"{cfg}\" > {slot_cfg};'".format(
        logs_dir=logs_dir,
        env_txt=env_txt,
        cfg=escaped_cmd,
        slot_cfg=slot_cfg,
    )

    nodes.execute_async(cmd, check_retcode=False, nodes=[node])


def _deploy_slot_configs(components, nodes, cluster_details, walle_provider):
    if 'dynamic_slots' not in components:
        return

    slots_per_domain = _get_available_slots(components, nodes, cluster_details, walle_provider)[0]
    for domain in cluster_details.domains:
        slots_taken = set()
        available_slots_per_zone = slots_per_domain[domain.domain_name]
        for tenant in domain.tenants:
            for compute_unit in tenant.compute_units:
                zone = compute_unit.zone.lower()
                for _ in range(compute_unit.count):
                    try:
                        while True:
                            slot, node = available_slots_per_zone[zone].popleft()
                            if (slot, node) in slots_taken:
                                continue
                            slots_taken.add((slot, node))
                            _deploy_slot_config_for_tenant(nodes, slot, tenant, node)
                            break
                    except IndexError:
                        logger.critical('insufficient slots allocated')
                        return


def _start_slot(nodes, slot):
    cmd = "sudo sh -c \"if [ -x /sbin/start ]; "\
          "    then start kikimr-multi slot={slot} tenant=dynamic mbus={mbus} grpc={grpc} mon={mon} ic={ic}; "\
          "    else systemctl start kikimr-multi@{slot}; fi\"".format(
              slot=slot.slot,
              mbus=slot.mbus,
              grpc=slot.grpc,
              mon=slot.mon,
              ic=slot.ic
          )
    nodes.execute_async(cmd, check_retcode=False)


def _start_slot_for_tenant(nodes, slot, tenant, host, node_bind=None):
    cmd = "sudo sh -c \"if [ -x /sbin/start ]; "\
          "    then start kikimr-multi slot={slot} tenant=/{domain}/{name} mbus={mbus} grpc={grpc} mon={mon} ic={ic}; "\
          "    else systemctl start kikimr-multi@{slot}; fi\"".format(
              slot=slot.slot,
              domain=slot.domain,
              name=tenant.name,
              mbus=slot.mbus,
              grpc=slot.grpc,
              mon=slot.mon,
              ic=slot.ic
          )
    if node_bind is not None:
        cmd += " bindnumanode={bind}".format(bind=node_bind)
    nodes.execute_async(cmd, check_retcode=False, nodes=[host])


def _start_all_slots(nodes):
    cmd = "find /Berkanavt/ -maxdepth 1 -type d  -name kikimr_* " \
          " | while read x; do " \
          "      sudo sh -c \"if [ -x /sbin/start ]; "\
          "          then start kikimr-multi slot=${x#/Berkanavt/kikimr_}; "\
          "          else systemctl start kikimr-multi@${x#/Berkanavt/kikimr_}; fi\"; " \
          "   done"
    nodes.execute_async(cmd, check_retcode=False)


def _start_static(nodes):
    nodes.execute_async("sudo service kikimr start", check_retcode=True)


def _start_dynamic(components, nodes, cluster_details, walle_provider):
    if 'dynamic_slots' in components:

        def get_numa_nodes(nodes):
            results = dict()
            nodes.execute_async("numactl --hardware | head -n 1 | awk '{print $2}'", check_retcode=False,
                                results=results)
            return {
                host: int(result['stdout']) if result['retcode'] == 0 else 0
                for host, result in results.items()
            }

        numa_nodes = None  # get_numa_nodes(nodes)
        numa_nodes_counters = {node: 0 for node in nodes.nodes_list}

        (slots_per_domain, all_available_slots_count,) = _get_available_slots(components, nodes, cluster_details, walle_provider)

        for domain in cluster_details.domains:

            slots_taken = set()
            available_slots_per_zone = slots_per_domain[domain.domain_name]

            if domain.bind_slots_to_numa_nodes and numa_nodes is None:
                numa_nodes = get_numa_nodes(nodes)

            for tenant in domain.tenants:
                for compute_unit in tenant.compute_units:
                    zone = compute_unit.zone.lower()
                    for _ in range(compute_unit.count):
                        try:
                            while True:
                                slot, node = available_slots_per_zone[zone].popleft()
                                if (slot, node) in slots_taken:
                                    continue
                                slots_taken.add((slot, node))
                                if domain.bind_slots_to_numa_nodes and numa_nodes[node] > 0:
                                    _start_slot_for_tenant(
                                        nodes,
                                        slot,
                                        tenant,
                                        host=node,
                                        node_bind=numa_nodes_counters[node]
                                    )
                                    numa_nodes_counters[node] += 1
                                    numa_nodes_counters[node] %= numa_nodes[node]
                                else:
                                    _start_slot_for_tenant(nodes, slot, tenant, host=node)
                                break
                        except IndexError:
                            logger.critical('insufficient slots allocated')
                            return

            logger.warning('{count} unused slots'.format(count=all_available_slots_count - len(slots_taken)))


def slice_start(components, nodes, cluster_details, walle_provider):
    if 'kikimr' in components:
        _start_static(nodes)

    _start_dynamic(components, nodes, cluster_details, walle_provider)


def _stop_all_slots(nodes):
    cmd = "find /Berkanavt/ -maxdepth 1 -type d  -name kikimr_* " \
          " | while read x; do " \
          "      sudo sh -c \"if [ -x /sbin/stop ]; "\
          "          then stop kikimr-multi slot=${x#/Berkanavt/kikimr_}; "\
          "          else systemctl stop kikimr-multi@${x#/Berkanavt/kikimr_}; fi\"; " \
          "   done"
    nodes.execute_async(cmd, check_retcode=False)


def _stop_slot_ret(nodes, slot):
    cmd = "sudo sh -c \"if [ -x /sbin/stop ]; "\
          "    then stop kikimr-multi slot={slot}; "\
          "    else systemctl stop kikimr-multi@{slot}; fi\"".format(
              slot=slot.slot,
          )
    return nodes.execute_async_ret(cmd, check_retcode=False)


def _stop_slot(nodes, slot):
    tasks = _stop_slot_ret(nodes, slot)
    nodes._check_async_execution(tasks, False)


def _stop_static(nodes):
    nodes.execute_async("sudo service kikimr stop", check_retcode=False)


def _stop_dynamic(components, nodes, cluster_details):
    if 'dynamic_slots' in components:
        tasks = []
        for slot in cluster_details.dynamic_slots.values():
            tasks_slot = _stop_slot_ret(nodes, slot)
            for task in tasks_slot:
                tasks.append(task)
        nodes._check_async_execution(tasks, False)


def slice_stop(components, nodes, cluster_details, walle_provider):
    _stop_dynamic(components, nodes, cluster_details)

    if 'kikimr' in components:
        _stop_static(nodes)


slice_kikimr_path = '/Berkanavt/kikimr/bin/kikimr'
slice_cfg_path = '/Berkanavt/kikimr/cfg'
slice_secrets_path = '/Berkanavt/kikimr/token'


def _update_kikimr(nodes, bin_path, compressed_path):
    bin_directory = os.path.dirname(bin_path)
    nodes.copy(bin_path, slice_kikimr_path, compressed_path=compressed_path)
    for lib in ['libiconv.so', 'liblibaio-dynamic.so', 'liblibidn-dynamic.so']:
        lib_path = os.path.join(bin_directory, lib)
        if os.path.exists(lib_path):
            remote_lib_path = os.path.join('/lib', lib)
            nodes.copy(lib_path, remote_lib_path)


def _update_cfg(nodes, cfg_path):
    nodes.copy(cfg_path, slice_cfg_path, directory=True)


def _deploy_secrets(nodes, yav_version):
    if not yav_version:
        return

    nodes.execute_async(
        "sudo bash -c 'set -o pipefail && sudo mkdir -p {secrets} && "
        "yav get version {yav_version} -o auth_file | sudo tee {auth}'".format(
            yav_version=yav_version,
            secrets=slice_secrets_path,
            auth=os.path.join(slice_secrets_path, 'kikimr.token')
        )
    )

    # creating symlinks, to attach auth.txt to node
    nodes.execute_async(
        "sudo ln -f {secrets_auth} {cfg_auth}".format(
            secrets_auth=os.path.join(slice_secrets_path, 'kikimr.token'),
            cfg_auth=os.path.join(slice_cfg_path, 'auth.txt')
        )
    )

    nodes.execute_async(
        "sudo bash -c 'set -o pipefail && yav get version {yav_version} -o tvm_secret |  sudo tee {tvm_secret}'".format(
            yav_version=yav_version,
            tvm_secret=os.path.join(slice_secrets_path, 'tvm_secret')
        )
    )


def slice_update(components, nodes, cluster_details, configurator, do_clear_logs, args, walle_provider):
    if do_clear_logs:
        _clear_logs(nodes)

    if 'kikimr' in components:
        if 'bin' in components.get('kikimr', []):
            _update_kikimr(nodes, configurator.kikimr_bin, configurator.kikimr_compressed_bin)

    slice_stop(components, nodes, cluster_details, walle_provider)
    if 'kikimr' in components:
        if 'cfg' in components.get('kikimr', []):
            static = configurator.create_static_cfg()
            _update_cfg(nodes, static)
            _deploy_secrets(nodes, args.yav_version)

    _deploy_slot_configs(components, nodes, cluster_details, walle_provider)
    slice_start(components, nodes, cluster_details, walle_provider)


def slice_update_raw_configs(components, nodes, cluster_details, config_path, walle_provider):
    slice_stop(components, nodes, cluster_details, walle_provider)
    if 'kikimr' in components:
        if 'cfg' in components.get('kikimr', []):
            kikimr_cfg = os.path.join(config_path, 'kikimr-static')
            _update_cfg(nodes, kikimr_cfg)

    slice_start(components, nodes, cluster_details, walle_provider)
