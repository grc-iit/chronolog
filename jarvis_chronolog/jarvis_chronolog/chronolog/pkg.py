"""
ChronoLog deployment package.

ChronoLog is a distributed, tiered shared log store that leverages physical
time to provide total ordering of events. It consists of four chimod
runtimes: keeper, grapher, player, and store.

Supports bare-metal (default) and container deployment via deploy_mode.
"""
from jarvis_cd.core.pkg import Service
from jarvis_cd.shell import Exec, PsshExecInfo, LocalExecInfo
from jarvis_cd.shell.process import Kill, Rm, Mkdir
import json
import os
import yaml


class Chronolog(Service):
    """
    ChronoLog deployment service. Deploys ChronoLog chimods into a running
    IOWarp (Chimaera) runtime via chimaera compose.
    """

    @property
    def server_conf_path(self):
        return os.path.join(self.shared_dir, 'chronolog_conf.json')

    @property
    def client_conf_path(self):
        return os.path.join(self.shared_dir, 'chronolog_client_conf.json')

    @property
    def compose_conf_path(self):
        return os.path.join(self.shared_dir, 'chronolog_compose.yaml')

    # ------------------------------------------------------------------
    # Menu
    # ------------------------------------------------------------------

    def _configure_menu(self):
        return [
            {
                'name': 'deploy_mode',
                'msg': 'Deployment mode',
                'type': str,
                'choices': ['default', 'container'],
                'default': 'default',
            },
            {
                'name': 'visor_ip',
                'msg': 'IP address for ChronoVisor services',
                'type': str,
                'default': '127.0.0.1',
            },
            {
                'name': 'visor_client_port',
                'msg': 'Base port for VisorClientPortalService',
                'type': int,
                'default': 5555,
            },
            {
                'name': 'keeper_port',
                'msg': 'Base port for KeeperRecordingService',
                'type': int,
                'default': 6666,
            },
            {
                'name': 'keeper_admin_port',
                'msg': 'Base port for KeeperDataStoreAdminService',
                'type': int,
                'default': 7777,
            },
            {
                'name': 'visor_keeper_port',
                'msg': 'Base port for VisorKeeperRegistryService',
                'type': int,
                'default': 8888,
            },
            {
                'name': 'grapher_drain_port',
                'msg': 'Base port for KeeperGrapherDrainService',
                'type': int,
                'default': 3333,
            },
            {
                'name': 'grapher_admin_port',
                'msg': 'Base port for GrapherDataStoreAdminService',
                'type': int,
                'default': 4444,
            },
            {
                'name': 'player_admin_port',
                'msg': 'Base port for PlayerStoreAdminService',
                'type': int,
                'default': 2222,
            },
            {
                'name': 'player_query_port',
                'msg': 'Base port for PlaybackQueryService',
                'type': int,
                'default': 2225,
            },
            {
                'name': 'client_query_port',
                'msg': 'Base port for ClientQueryService',
                'type': int,
                'default': 5557,
            },
            {
                'name': 'protocol',
                'msg': 'RPC protocol for Margo/Mercury',
                'type': str,
                'default': 'ofi+sockets',
            },
            {
                'name': 'recording_group',
                'msg': 'Recording group ID',
                'type': int,
                'default': 7,
            },
            {
                'name': 'max_story_chunk_size',
                'msg': 'Maximum story chunk size in bytes',
                'type': int,
                'default': 4096,
            },
            {
                'name': 'story_chunk_duration_secs',
                'msg': 'Duration of each story chunk in seconds',
                'type': int,
                'default': 10,
            },
            {
                'name': 'acceptance_window_secs',
                'msg': 'Acceptance window in seconds',
                'type': int,
                'default': 15,
            },
            {
                'name': 'inactive_story_delay_secs',
                'msg': 'Delay before inactive story cleanup in seconds',
                'type': int,
                'default': 120,
            },
            {
                'name': 'story_files_dir',
                'msg': 'Directory for story data files',
                'type': str,
                'default': '/tmp',
            },
            {
                'name': 'log_level',
                'msg': 'Logging level',
                'type': str,
                'choices': ['debug', 'info', 'warning', 'error'],
                'default': 'info',
            },
            {
                'name': 'log_filesize',
                'msg': 'Maximum log file size in bytes',
                'type': int,
                'default': 1048576,
            },
            {
                'name': 'log_filenum',
                'msg': 'Number of rotated log files',
                'type': int,
                'default': 3,
            },
            {
                'name': 'delayed_admin_exit_secs',
                'msg': 'Delay before admin exit in seconds (visor)',
                'type': int,
                'default': 3,
            },
        ]

    # ------------------------------------------------------------------
    # Container phases
    # ------------------------------------------------------------------

    def _build_phase(self):
        """
        Return the build.sh script that installs ChronoLog + IOWarp into
        the shared jarvis pipeline build container (jarvis-cd's
        single-build-container model; see
        jarvis_cd.core.pipeline._build_pipeline_container).
        """
        if self.config.get('deploy_mode') != 'container':
            return None
        base = getattr(self.pipeline, 'container_base', 'ubuntu:24.04')
        content = self._read_build_script('build.sh', {
            'BASE_IMAGE': base,
        })
        return content, 'chronolog'

    def _build_deploy_phase(self):
        if self.config.get('deploy_mode') != 'container':
            return None
        base = getattr(self.pipeline, 'container_base', 'ubuntu:24.04')
        suffix = getattr(self, '_build_suffix', '')
        content = self._read_template('Dockerfile.deploy', {
            'BUILD_IMAGE': self.build_image_name(),
            'BASE_IMAGE': base,
        })
        return content, suffix

    # ------------------------------------------------------------------
    # Configuration
    # ------------------------------------------------------------------

    def _configure(self, **kwargs):
        super()._configure(**kwargs)

        # Set environment variables for ChronoLog
        self.setenv('CHRONOLOG_CONF', self.server_conf_path)
        self.setenv('CHRONOLOG_CLIENT_CONF', self.client_conf_path)

        if self.config.get('deploy_mode') == 'default':
            # Ensure story files directory exists on all nodes
            story_dir = self.config['story_files_dir']
            if self.hostfile.hosts:
                Mkdir(story_dir,
                      PsshExecInfo(env=self.mod_env,
                                   hostfile=self.hostfile)).run()
            else:
                Mkdir(story_dir, LocalExecInfo(env=self.mod_env)).run()

        # Generate all configuration files
        self._generate_server_conf()
        self._generate_client_conf()
        self._generate_compose_conf()

        self.log("ChronoLog configured")
        self.log(f"  Server config: {self.server_conf_path}")
        self.log(f"  Client config: {self.client_conf_path}")
        self.log(f"  Compose config: {self.compose_conf_path}")

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    def start(self):
        if not os.path.exists(self.compose_conf_path):
            self.log("Error: ChronoLog not configured. Run configure first.")
            return

        cmd = f'chimaera compose {self.compose_conf_path}'
        self.log("Starting ChronoLog services via chimaera compose...")
        self.log(f"  Running: {cmd}")
        self.log(f"  Nodes: {len(self.hostfile)}")

        Exec(cmd, PsshExecInfo(
            env=self.env,
            hostfile=self.hostfile,
            container=self._container_engine,
            container_image=self.deploy_image_name(),
            private_dir=self.private_dir,
            bind_mounts=self.container_mounts,
        )).run()

        self.sleep()
        self.log("ChronoLog services started successfully on all nodes")

    def stop(self):
        self.log("ChronoLog modules will stop when the chimaera runtime stops")

    def kill(self):
        self.log("Forcibly killing ChronoLog-related processes on all nodes")
        Kill('chronolog', PsshExecInfo(
            hostfile=self.hostfile,
            container=self._container_engine,
            container_image=self.deploy_image_name(),
            private_dir=self.private_dir,
            bind_mounts=self.container_mounts,
        ), partial=True).run()

    def status(self):
        return "unknown"

    def clean(self):
        self.log("Cleaning ChronoLog data")

        for path in [self.server_conf_path, self.client_conf_path,
                     self.compose_conf_path]:
            if path and os.path.exists(path):
                os.remove(path)

        story_dir = self.config.get('story_files_dir', '/tmp')
        rm_path = os.path.join(story_dir, 'chrono_*')
        if self.hostfile.hosts:
            Rm(rm_path,
               PsshExecInfo(env=self.env,
                            hostfile=self.hostfile)).run()
        else:
            Rm(rm_path, LocalExecInfo(env=self.env)).run()

        self.log("ChronoLog cleanup completed")

    # ------------------------------------------------------------------
    # Config generation
    # ------------------------------------------------------------------

    def _generate_server_conf(self):
        c = self.config
        ip = c['visor_ip']
        proto = c['protocol']

        conf = {
            'clock': {
                'clocksource_type': 'CPP_STYLE',
                'drift_cal_sleep_sec': 10,
                'drift_cal_sleep_nsec': 0
            },
            'authentication': {
                'auth_type': 'RBAC',
                'module_location': '/path/to/auth_module'
            },
            'chrono_visor': {
                'VisorClientPortalService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['visor_client_port'],
                        'service_provider_id': 55
                    }
                },
                'VisorKeeperRegistryService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['visor_keeper_port'],
                        'service_provider_id': 88
                    }
                },
                'Monitoring': {
                    'monitor': {
                        'type': 'file',
                        'file': 'chrono-visor-1.log',
                        'level': c['log_level'],
                        'name': 'ChronoVisor',
                        'filesize': 104857600,
                        'filenum': c['log_filenum'],
                        'flushlevel': 'warning'
                    }
                },
                'delayed_data_admin_exit_in_secs': c['delayed_admin_exit_secs']
            },
            'chrono_keeper': {
                'RecordingGroup': c['recording_group'],
                'KeeperRecordingService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['keeper_port'],
                        'service_provider_id': 66
                    }
                },
                'KeeperDataStoreAdminService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['keeper_admin_port'],
                        'service_provider_id': 77
                    }
                },
                'VisorKeeperRegistryService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['visor_keeper_port'],
                        'service_provider_id': 88
                    }
                },
                'KeeperGrapherDrainService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['grapher_drain_port'],
                        'service_provider_id': 33
                    }
                },
                'Monitoring': {
                    'monitor': {
                        'type': 'file',
                        'file': 'chrono-keeper-1.log',
                        'level': c['log_level'],
                        'name': 'ChronoKeeper',
                        'filesize': c['log_filesize'],
                        'filenum': c['log_filenum'],
                        'flushlevel': 'warning'
                    }
                },
                'DataStoreInternals': {
                    'max_story_chunk_size': c['max_story_chunk_size'],
                    'story_chunk_duration_secs': c['story_chunk_duration_secs'],
                    'acceptance_window_secs': c['acceptance_window_secs'],
                    'inactive_story_delay_secs': c['inactive_story_delay_secs']
                },
                'Extractors': {
                    'story_files_dir': c['story_files_dir']
                }
            },
            'chrono_grapher': {
                'RecordingGroup': c['recording_group'],
                'KeeperGrapherDrainService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['grapher_drain_port'],
                        'service_provider_id': 33
                    }
                },
                'DataStoreAdminService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['grapher_admin_port'],
                        'service_provider_id': 44
                    }
                },
                'VisorRegistryService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['visor_keeper_port'],
                        'service_provider_id': 88
                    }
                },
                'Monitoring': {
                    'monitor': {
                        'type': 'file',
                        'file': 'chrono-grapher-1.log',
                        'level': c['log_level'],
                        'name': 'ChronoGrapher',
                        'filesize': c['log_filesize'],
                        'filenum': c['log_filenum'],
                        'flushlevel': c['log_level']
                    }
                },
                'DataStoreInternals': {
                    'max_story_chunk_size': c['max_story_chunk_size'],
                    'story_chunk_duration_secs': 60,
                    'acceptance_window_secs': 180,
                    'inactive_story_delay_secs': 300
                },
                'Extractors': {
                    'story_files_dir': c['story_files_dir']
                }
            },
            'chrono_player': {
                'RecordingGroup': c['recording_group'],
                'PlayerStoreAdminService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['player_admin_port'],
                        'service_provider_id': 22
                    }
                },
                'PlaybackQueryService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['player_query_port'],
                        'service_provider_id': 25
                    }
                },
                'VisorRegistryService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['visor_keeper_port'],
                        'service_provider_id': 88
                    }
                },
                'Monitoring': {
                    'monitor': {
                        'type': 'file',
                        'file': 'chrono-player-1.log',
                        'level': c['log_level'],
                        'name': 'ChronoPlayer',
                        'filesize': c['log_filesize'],
                        'filenum': c['log_filenum'],
                        'flushlevel': c['log_level']
                    }
                },
                'DataStoreInternals': {
                    'max_story_chunk_size': c['max_story_chunk_size'],
                    'story_chunk_duration_secs': 60,
                    'acceptance_window_secs': 180,
                    'inactive_story_delay_secs': 300
                },
                'ArchiveReaders': {
                    'story_files_dir': c['story_files_dir']
                }
            }
        }

        with open(self.server_conf_path, 'w') as f:
            json.dump(conf, f, indent=2)

    def _generate_client_conf(self):
        c = self.config
        ip = c['visor_ip']
        proto = c['protocol']

        conf = {
            'chrono_client': {
                'VisorClientPortalService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['visor_client_port'],
                        'service_provider_id': 55
                    }
                },
                'ClientQueryService': {
                    'rpc': {
                        'protocol_conf': proto,
                        'service_ip': ip,
                        'service_base_port': c['client_query_port'],
                        'service_provider_id': 57
                    }
                },
                'Monitoring': {
                    'monitor': {
                        'type': 'file',
                        'file': 'chrono-client.log',
                        'level': c['log_level'],
                        'name': 'ChronoClient',
                        'filesize': c['log_filesize'],
                        'filenum': c['log_filenum'],
                        'flushlevel': 'warning'
                    }
                }
            }
        }

        with open(self.client_conf_path, 'w') as f:
            json.dump(conf, f, indent=2)

    def _generate_compose_conf(self):
        compose_list = [
            {
                'mod_name': 'chronolog_keeper',
                'pool_name': 'chronolog_keeper',
                'pool_query': 'local',
                'pool_id': 600.0,
            },
            {
                'mod_name': 'chronolog_grapher',
                'pool_name': 'chronolog_grapher',
                'pool_query': 'local',
                'pool_id': 601.0,
            },
            {
                'mod_name': 'chronolog_player',
                'pool_name': 'chronolog_player',
                'pool_query': 'local',
                'pool_id': 602.0,
            },
            {
                'mod_name': 'chronolog_store',
                'pool_name': 'chronolog_store',
                'pool_query': 'local',
                'pool_id': 603.0,
            },
        ]

        with open(self.compose_conf_path, 'w') as f:
            yaml.dump({'compose': compose_list}, f, default_flow_style=False)
