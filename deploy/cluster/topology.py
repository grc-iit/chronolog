NODES = {'dragon': '100.101.232.95', 'blade': '100.124.181.9', 'mini': '100.74.131.112'}
TABLE = [dict(node=node, ip=ip, replica=index + 1, keeper=f'keeper-{index + 1}',
              visor=f'visor-{index + 1}', player=f'player-{index + 1}',
              grapher={'dragon': 'grapher-a', 'mini': 'grapher-b'}.get(node))
         for index, (node, ip) in enumerate(NODES.items())]
PORTS = {row['node']: [50051, 50061, 50071, 50052, 50062, 50054]
         + ([50053] if row['grapher'] else []) for row in TABLE}
ARCHIVE = '/mnt/nfs/chronolog-sprint/archive'


def configs(home, tag):
    peers = [dict(id=r['replica'], catalog_endpoint=r['ip'] + ':50051',
                  internal_endpoint=r['ip'] + ':50061', raft_endpoint=r['ip'] + ':50071') for r in TABLE]
    keepers = [dict(process_id=r['keeper'], endpoint=r['ip'] + ':50052') for r in TABLE]
    graphers = [r['ip'] + ':50053' for r in TABLE if r['grapher']]
    catalog = ','.join(p['catalog_endpoint'] for p in peers)
    internal = ','.join(p['internal_endpoint'] for p in peers)
    result = {}
    for r in TABLE:
        node, ip = r['node'], r['ip']
        folder = f"{home[node]}/chronolog-sprint/run/{tag}"
        result[r['visor']] = (node, dict(membership_mode='dynamic', listen=ip + ':50051',
            internal_listen=ip + ':50061', db_path=folder + '/catalog.sqlite', keepers=keepers,
            graphers=graphers, player=TABLE[0]['ip'] + ':50054', worker_threads=4,
            raft=dict(server_id=r['replica'], raft_endpoint=ip + ':50071', peers=peers)))
        result[r['keeper']] = (node, dict(process_id=r['keeper'], listen=ip + ':50052',
            internal_listen=ip + ':50062', self_endpoint=ip + ':50052', visor_internal=internal,
            wal_dir=folder + '/' + r['keeper'] + '/wal', story_chunk_duration_secs=1,
            seal_interval_ms=200, archive_visibility_delay_secs=1, watermark_resend_timeout_secs=2,
            shutdown_confirm_timeout_secs=5, worker_threads=4, heartbeat_interval_ms=200))
        result[r['player']] = (node, dict(listen=ip + ':50054', advertise=ip + ':50054',
            visor=catalog, visor_internal=internal, archive_root=ARCHIVE, manifest_poll_ms=200,
            keeper_internal={k['keeper']: k['ip'] + ':50062' for k in TABLE}))
        if r['grapher']:
            result[r['grapher']] = (node, dict(process_id=r['grapher'], manifest_writer=r['grapher'],
                internal_listen=ip + ':50053', self_endpoint=ip + ':50053', visor_internal=internal,
                archive_root=ARCHIVE))
    return result
