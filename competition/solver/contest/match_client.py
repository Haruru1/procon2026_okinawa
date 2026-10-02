"""Match supervisor. Daily routing and match physics remain in the frozen C++ engine."""

from __future__ import annotations

import argparse
import copy
from dataclasses import dataclass
from email.utils import parsedate_to_datetime
import hashlib
import heapq
import http.client
import ipaddress
import itertools
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid


HERE = Path(__file__).resolve().parent
REPO = (
    Path(sys.executable).resolve().parent
    if getattr(sys, 'frozen', False)
    else HERE.parents[2]
)


def default_engine():
    if getattr(sys, 'frozen', False):
        return Path(sys.executable).resolve().parent / 'contest_solver.exe'
    return HERE.parent / 'build' / 'contest_solver.exe'


def team_lock_dir():
    # Copies of the portable package must still share one same-user/token lock.
    base = (
        Path(os.environ['LOCALAPPDATA'])
        if os.name == 'nt' and os.environ.get('LOCALAPPDATA')
        else Path.home() / '.local' / 'state'
    )
    return base / 'HexaUdon' / 'match_locks'


DEFAULT_ENGINE = default_engine()


class MatchError(RuntimeError):
    pass


class TransportError(MatchError):
    pass


class WindowClosed(MatchError):
    pass


class EngineError(MatchError):
    pass


def digest(value):
    return hashlib.sha256(
        json.dumps(
            value,
            sort_keys=True,
            separators=(',', ':'),
        ).encode()
    ).hexdigest()


def atomic_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(
        path.name + '.' + uuid.uuid4().hex + '.tmp'
    )

    try:
        with temporary.open(
            'w',
            encoding='utf-8',
            newline='\n',
        ) as f:
            json.dump(
                value,
                f,
                ensure_ascii=True,
                indent=2,
                allow_nan=False,
            )
            f.flush()
            os.fsync(f.fileno())

        os.replace(temporary, path)

    finally:
        temporary.unlink(missing_ok=True)


# ============================================================
# Supply placement selection
# ============================================================
#
# --supply-count N の N 自体は従来どおり利用者が決める。
#
# 変更前:
#   末尾 N 台を補給車にする。
#
# 変更後:
#   N 台の全 Agent ID 組合せを列挙し、
#   初期配置上もっとも補給しやすい組合せを選ぶ。
#
# 比較順:
#   1. 各巡回車から「最寄り補給車」までの距離の最大値を最小化
#   2. その距離の合計を最小化
#   3. 補給車から Spot 群への最寄り距離合計を最小化
#   4. 完全同点なら Agent ID が小さい組を採用
#
# タイプ選択時点では Day1 の道路は順調なので、
# road の移動時間は 1 step として計算する。
#
# これはタイプ選択用の軽量評価だけであり、
# 実際の日次経路は従来どおり C++ solver が計算する。
# ============================================================


_MOVE_STEPS = {
    0: 2,  # flat
    1: 1,  # road: clear on Day 1
    2: 3,  # mountain
}


def _flatten_map_cells(map_data):
    try:
        height = int(map_data['height'])
        width = int(map_data['width'])
        cells = map_data['cells']
    except (KeyError, TypeError, ValueError) as e:
        raise MatchError('malformed map for supply placement') from e

    if (
        isinstance(cells, list)
        and cells
        and isinstance(cells[0], list)
    ):
        flat = [
            int(value)
            for row in cells
            for value in row
        ]
    else:
        try:
            flat = [int(value) for value in cells]
        except (TypeError, ValueError) as e:
            raise MatchError(
                'malformed map cells for supply placement'
            ) from e

    if (
        height <= 0
        or width <= 0
        or len(flat) != height * width
    ):
        raise MatchError(
            'map dimensions/cells mismatch for supply placement'
        )

    if any(value not in (0, 1, 2, 3) for value in flat):
        raise MatchError(
            'unknown terrain in supply placement map'
        )

    return height, width, flat


def _hex_neighbors(pos, height, width):
    """
    Hex grid used by the competition.

    Even rows are shifted to the right.
    Only neighboring cell IDs are needed here;
    direction numbers are not needed.
    """

    row, col = divmod(pos, width)

    candidates = [
        (row, col - 1),
        (row, col + 1),
    ]

    if row % 2 == 0:
        # Even row is shifted right.
        candidates.extend([
            (row - 1, col),
            (row - 1, col + 1),
            (row + 1, col),
            (row + 1, col + 1),
        ])
    else:
        candidates.extend([
            (row - 1, col - 1),
            (row - 1, col),
            (row + 1, col - 1),
            (row + 1, col),
        ])

    for next_row, next_col in candidates:
        if (
            0 <= next_row < height
            and 0 <= next_col < width
        ):
            yield next_row * width + next_col


def _shortest_step_distances(setting, start_pos):
    """
    start_pos から全セルまでの最短 step 数を求める。

    補給車は燃料を消費しないため、
    タイプ選択では移動時間だけを見る。

    移動時間は移動元セルの地形で決まる。
    Day1 の道路状態は clear とする。
    """

    if not isinstance(setting, dict):
        raise MatchError(
            'malformed setting for supply placement'
        )

    map_data = setting.get('map')

    if not isinstance(map_data, dict):
        raise MatchError(
            'setting has no valid map for supply placement'
        )

    height, width, cells = _flatten_map_cells(map_data)

    cell_count = height * width

    if (
        type(start_pos) is not int
        or not 0 <= start_pos < cell_count
    ):
        raise MatchError(
            'agent start position is outside the map'
        )

    if cells[start_pos] == 3:
        raise MatchError(
            'agent starts on an impassable pond'
        )

    inf = math.inf

    dist = [inf] * cell_count
    dist[start_pos] = 0

    queue = [(0, start_pos)]

    while queue:
        current_distance, current = heapq.heappop(queue)

        if current_distance != dist[current]:
            continue

        terrain = cells[current]

        if terrain == 3:
            continue

        move_steps = _MOVE_STEPS.get(terrain)

        if move_steps is None:
            raise MatchError(
                'unknown terrain during supply placement'
            )

        for nxt in _hex_neighbors(
            current,
            height,
            width,
        ):
            # Cannot enter a pond.
            if cells[nxt] == 3:
                continue

            candidate = current_distance + move_steps

            if candidate < dist[nxt]:
                dist[nxt] = candidate
                heapq.heappush(
                    queue,
                    (candidate, nxt),
                )

    return dist


def choose_supply_layout(setting, supply_count):
    """
    supply_count を固定したまま、
    最も良い Supply Agent ID の集合を全列挙で選択する。

    Returns:
        kinds:
            original Agent order の 0/1 配列。
            0 = patrol
            1 = supply

        diagnostics:
            events.jsonl に記録する診断情報。
    """

    agents = setting.get('agents')

    if not isinstance(agents, list):
        raise MatchError(
            'malformed agent list for supply placement'
        )

    agent_positions = []

    for pos in agents:
        if type(pos) is not int:
            raise MatchError(
                'agent position must be an integer'
            )
        agent_positions.append(pos)

    agent_count = len(agent_positions)

    if (
        type(supply_count) is not int
        or not 0 <= supply_count <= agent_count
    ):
        raise MatchError(
            'supply-count outside agent count'
        )

    # No supplies.
    if supply_count == 0:
        return (
            [0] * agent_count,
            dict(
                policy='initial_position_minimax',
                supply_count=0,
                supply_ids=[],
                max_nearest_patrol_steps=None,
                sum_nearest_patrol_steps=None,
                sum_nearest_spot_steps=None,
                combinations=1,
            ),
        )

    # All agents are supplies.
    # There is only one possible layout.
    if supply_count == agent_count:
        return (
            [1] * agent_count,
            dict(
                policy='initial_position_minimax',
                supply_count=supply_count,
                supply_ids=list(range(agent_count)),
                max_nearest_patrol_steps=0,
                sum_nearest_patrol_steps=0,
                sum_nearest_spot_steps=None,
                combinations=1,
            ),
        )

    # Calculate a shortest-path table once for each Agent start.
    #
    # Max agent count is very small, so this is cheap compared with
    # the daily C++ solver.
    distance_from_agent = [
        _shortest_step_distances(
            setting,
            start_pos,
        )
        for start_pos in agent_positions
    ]

    spots = setting.get('spots', [])

    if not isinstance(spots, list):
        raise MatchError(
            'malformed spots for supply placement'
        )

    spot_positions = []

    for spot in spots:
        if not isinstance(spot, dict):
            raise MatchError(
                'malformed spot for supply placement'
            )

        pos = spot.get('pos')

        if type(pos) is not int:
            raise MatchError(
                'spot position must be an integer'
            )

        spot_positions.append(pos)

    best_ids = None
    best_key = None

    combination_count = 0

    all_agent_ids = range(agent_count)

    for supply_ids in itertools.combinations(
        all_agent_ids,
        supply_count,
    ):
        combination_count += 1

        supply_set = set(supply_ids)

        patrol_ids = [
            agent_id
            for agent_id in all_agent_ids
            if agent_id not in supply_set
        ]

        nearest_patrol_distances = []

        reachable = True

        # Primary metric:
        # for every patrol start, find the nearest selected supply start.
        for patrol_id in patrol_ids:
            patrol_position = agent_positions[patrol_id]

            nearest = min(
                distance_from_agent[supply_id][
                    patrol_position
                ]
                for supply_id in supply_ids
            )

            if not math.isfinite(nearest):
                reachable = False
                break

            nearest_patrol_distances.append(
                int(nearest)
            )

        if not reachable:
            continue

        max_patrol_distance = max(
            nearest_patrol_distances,
            default=0,
        )

        sum_patrol_distance = sum(
            nearest_patrol_distances
        )

        # Tie breaker:
        # if two supply layouts cover patrol starts equally well,
        # prefer the one that is also central to the Spot set.
        sum_spot_distance = 0

        for spot_position in spot_positions:
            nearest = min(
                distance_from_agent[supply_id][
                    spot_position
                ]
                for supply_id in supply_ids
            )

            if not math.isfinite(nearest):
                reachable = False
                break

            sum_spot_distance += int(nearest)

        if not reachable:
            continue

        key = (
            max_patrol_distance,
            sum_patrol_distance,
            sum_spot_distance,
            supply_ids,
        )

        if best_key is None or key < best_key:
            best_key = key
            best_ids = supply_ids

    if best_ids is None:
        raise MatchError(
            'no reachable supply layout found'
        )

    best_set = set(best_ids)

    kinds = [
        1 if agent_id in best_set else 0
        for agent_id in range(agent_count)
    ]

    diagnostics = dict(
        policy='initial_position_minimax',
        supply_count=supply_count,
        supply_ids=list(best_ids),
        max_nearest_patrol_steps=best_key[0],
        sum_nearest_patrol_steps=best_key[1],
        sum_nearest_spot_steps=best_key[2],
        combinations=combination_count,
    )

    return kinds, diagnostics


class Lock:
    def __init__(self, path):
        self.path, self.file = Path(path), None

    def __enter__(self):
        self.path.parent.mkdir(
            parents=True,
            exist_ok=True,
        )

        self.file = self.path.open('a+b')

        self.file.write(b'0')
        self.file.flush()
        self.file.seek(0)

        try:
            if os.name == 'nt':
                import msvcrt

                msvcrt.locking(
                    self.file.fileno(),
                    msvcrt.LK_NBLCK,
                    1,
                )
            else:
                import fcntl

                fcntl.flock(
                    self.file,
                    fcntl.LOCK_EX | fcntl.LOCK_NB,
                )

        except OSError as e:
            self.file.close()

            raise MatchError(
                'another client owns this run or team token'
            ) from e

        return self

    def __exit__(self, *_):
        if self.file:
            self.file.close()


def endpoint(value, allow_remote=False):
    u = urllib.parse.urlsplit(value)

    if (
        u.scheme not in ('http', 'https')
        or not u.hostname
        or u.username
        or u.password
        or u.query
        or u.fragment
    ):
        raise MatchError(
            'base URL must be an explicit http(s) origin '
            'without credentials/query'
        )

    if u.path not in ('', '/'):
        raise MatchError(
            'documented endpoints are relative to the server root'
        )

    try:
        local = ipaddress.ip_address(
            u.hostname
        ).is_loopback

    except ValueError:
        local = u.hostname.lower() == 'localhost'

    if not local and not allow_remote:
        raise MatchError(
            'non-loopback requires explicit --allow-remote; '
            'tests never use it'
        )

    return urllib.parse.urlunsplit(
        (
            u.scheme,
            u.netloc,
            '',
            '',
            '',
        )
    )


class NoRedirect(
    urllib.request.HTTPRedirectHandler
):
    def redirect_request(
        self,
        *_args,
        **_kwargs,
    ):
        return None


@dataclass
class Reply:
    status: int
    body: object
    received: float
    server_upper: float
    rtt: float
    clock_source: str

    def deadline(self, ends_at):
        return (
            self.received
            + float(ends_at)
            - self.server_upper
        )


class Api:
    def __init__(
        self,
        base_url,
        token,
        emit=lambda *_args, **_kw: None,
    ):
        self.base_url = base_url
        self.token = token
        self.emit = emit

        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}),
            NoRedirect(),
        )

        self.next_request = 0.0

    def request(
        self,
        method,
        path,
        body=None,
        deadline=None,
    ):
        delay = max(
            0.,
            self.next_request - time.monotonic(),
        )

        if (
            deadline is not None
            and time.monotonic() + delay + .75
            >= deadline
        ):
            raise WindowClosed(
                'insufficient time for a bounded HTTP request'
            )

        time.sleep(delay)

        timeout = (
            1.5
            if deadline is None
            else min(
                1.5,
                deadline
                - time.monotonic()
                - .25,
            )
        )

        started = time.monotonic()
        self.next_request = started + .25

        req = urllib.request.Request(
            self.base_url + path,
            method=method,
            data=(
                None
                if body is None
                else json.dumps(
                    body,
                    separators=(',', ':'),
                ).encode()
            ),
            headers={
                'Procon-Token': self.token,
                'Content-Type': 'application/json',
            },
        )

        try:
            try:
                response = self.opener.open(
                    req,
                    timeout=timeout,
                )

            except urllib.error.HTTPError as e:
                response = e

            with response:
                raw = response.read(
                    2 * 1024 * 1024 + 1
                )

                if len(raw) > 2 * 1024 * 1024:
                    raise ValueError(
                        'HTTP response exceeds limit'
                    )

                body = (
                    json.loads(raw)
                    if raw.strip()
                    else None
                )

                now = time.monotonic()
                header = response.headers.get('Date')

                upper = (
                    parsedate_to_datetime(
                        header
                    ).timestamp()
                    + 1
                    + now
                    - started
                    if header
                    else time.time() + 1
                )

                reply = Reply(
                    response.status,
                    body,
                    now,
                    upper,
                    now - started,
                    (
                        'http_date_upper_bound'
                        if header
                        else 'local_clock_assumption'
                    ),
                )

        except (
            OSError,
            urllib.error.URLError,
            ValueError,
            http.client.HTTPException,
        ) as e:
            self.emit(
                'transport_error',
                method=method,
                path=path,
                elapsed=time.monotonic() - started,
                error=type(e).__name__,
            )

            raise TransportError(
                type(e).__name__
            ) from e

        self.emit(
            'http',
            method=method,
            path=path,
            status=reply.status,
            rtt=reply.rtt,
            clock_source=reply.clock_source,
        )

        if reply.status == 401:
            raise MatchError(
                'authentication failed (401); '
                'check token without logging it'
            )

        return reply


class Engine:
    def __init__(
        self,
        executable,
        root,
        secret_env_keys=(),
    ):
        self.executable = Path(
            executable
        ).resolve()

        self.root = Path(root)

        self.secret_env_keys = {
            'PROCON_TOKEN',
            *secret_env_keys,
        }

    def evaluate(
        self,
        setting,
        daily,
        history,
        *,
        answer=None,
        budget=55.,
        timeout=56.,
    ):
        job = self.root / (
            'day_%d_%s'
            % (
                daily['day'],
                uuid.uuid4().hex,
            )
        )

        job.mkdir(parents=True)

        atomic_json(
            job / 'map.json',
            setting,
        )

        atomic_json(
            job / 'day.json',
            daily,
        )

        args = [
            str(self.executable),
            str(job / 'map.json'),
            str(job / 'day.json'),
            '--offline',
            '--report',
            str(job / 'report.json'),
        ]

        if history is not None:
            atomic_json(
                job / 'history.json',
                history,
            )

            args += [
                '--history',
                str(job / 'history.json'),
            ]

        if answer is not None:
            atomic_json(
                job / 'input_answer.json',
                answer,
            )

            args += [
                '--validate-answer',
                str(job / 'input_answer.json'),
            ]

        else:
            args += [
                '--budget-seconds',
                str(
                    max(
                        0.,
                        min(
                            55.,
                            budget,
                        ),
                    )
                ),
            ]

        start = time.monotonic()

        with (
            job / 'answer.json'
        ).open('wb') as out, (
            job / 'stderr.json'
        ).open('wb') as err:

            process = subprocess.Popen(
                args,
                stdout=out,
                stderr=err,
                env={
                    k: v
                    for k, v
                    in os.environ.items()
                    if k
                    not in self.secret_env_keys
                },
                creationflags=(
                    subprocess.CREATE_NO_WINDOW
                    if os.name == 'nt'
                    else 0
                ),
            )

            try:
                code = process.wait(
                    timeout=max(
                        .01,
                        timeout,
                    )
                )

            except BaseException:
                process.kill()
                process.wait()
                raise

        if code:
            raise EngineError(
                'C++ rejected input/plan or crashed; '
                'inspect '
                + str(job / 'stderr.json')
            )

        try:
            plan = json.loads(
                (
                    job / 'answer.json'
                ).read_text(
                    encoding='utf-8'
                )
            )

            report = json.loads(
                (
                    job / 'report.json'
                ).read_text(
                    encoding='utf-8'
                )
            )

            if (
                not report['replay']['valid']
                or not isinstance(plan, list)
            ):
                raise ValueError(
                    'invalid replay or answer shape'
                )

            if (
                answer is not None
                and plan != answer
            ):
                raise ValueError(
                    'validator silently changed actions'
                )

        except (
            OSError,
            ValueError,
            KeyError,
            TypeError,
        ) as e:
            raise EngineError(
                'missing/malformed C++ result in '
                + str(job)
            ) from e

        return dict(
            answer=plan,
            report=report,
            engine_seconds=(
                time.monotonic() - start
            ),
            job=str(job),
        )


def acknowledged(record):
    valid = [
        a
        for a in record['attempts']
        if a['status'] == 'accepted'
    ]

    return (
        max(
            valid,
            key=lambda a: a['revision'],
        )
        if valid
        else None
    )


def certain_plan(record):
    best = acknowledged(record)

    if best is None:
        raise MatchError(
            'day has no acknowledged answer; '
            'acquired history is unconfirmed'
        )

    for attempt in record['attempts']:
        if (
            attempt['status']
            in ('intent', 'unknown')
            and attempt['answer']
            != best['answer']
        ):
            raise MatchError(
                'ambiguous unacknowledged POST: '
                'cannot confirm history'
            )

    return best


def confirm_next(
    setting,
    record,
    next_day,
):
    if (
        next_day['day']
        != record['state']['day'] + 1
    ):
        raise MatchError(
            'skipped day; '
            'cannot infer intervening acquisitions'
        )

    best = certain_plan(record)

    final = (
        best['report']
        ['replay']
        ['committed_state']
        ['agents']
    )

    expected = [
        dict(
            kind=a['kind'],
            pos=f['cell'],
            fuel=f['fuel'],
        )
        for a, f
        in zip(
            record['state']['agents'],
            final,
        )
    ]

    if next_day['agents'] != expected:
        raise MatchError(
            'server next-day kinds/position/fuel '
            'disagrees with accepted replay'
        )

    history = copy.deepcopy(
        best['report']['next_history_draft']
    )

    if (
        history['map_config'] != setting
        or history['completed_days']
        != next_day['day']
    ):
        raise MatchError(
            'history belongs to another match/day'
        )

    history['confirmed'] = True

    return history


class MatchRunner:
    def __init__(
        self,
        base_url,
        token,
        run_dir,
        kinds=None,
        supply_count=None,
        engine=None,
        startup_timeout=600.,
        reserve=5.,
    ):
        self.root = Path(
            run_dir
        ).resolve()

        self.root.mkdir(
            parents=True,
            exist_ok=True,
        )

        self.path = (
            self.root / 'journal.json'
        )

        self.identity = dict(
            endpoint=base_url,
            token_sha256=hashlib.sha256(
                token.encode()
            ).hexdigest(),
        )

        self.state = (
            json.loads(
                self.path.read_text(
                    encoding='utf-8'
                )
            )
            if self.path.exists()
            else dict(
                format='c_staged.match_journal.v1',
                identity=self.identity,
                setting=None,
                kinds=None,
                type_accepted=False,
                days=[],
                history=None,
                status='new',
            )
        )

        if (
            self.state.get('format')
            != 'c_staged.match_journal.v1'
            or self.state.get('identity')
            != self.identity
        ):
            raise MatchError(
                'journal identity mismatch; '
                'never reuse another match/team journal'
            )

        self.api = Api(
            base_url,
            token,
            self.emit,
        )

        self.engine = (
            engine
            or Engine(
                DEFAULT_ENGINE,
                self.root / 'jobs',
            )
        )

        self.kinds_option = kinds
        self.supply_count = supply_count

        self.startup_timeout = (
            startup_timeout
        )

        self.reserve = reserve

    def emit(
        self,
        event,
        **fields,
    ):
        line = dict(
            time=time.time(),
            event=event,
            **fields,
        )

        with (
            self.root / 'events.jsonl'
        ).open(
            'a',
            encoding='utf-8',
        ) as f:
            f.write(
                json.dumps(
                    line,
                    ensure_ascii=True,
                    allow_nan=False,
                )
                + '\n'
            )

            f.flush()

        if event not in ('http',):
            print(
                json.dumps(
                    line,
                    ensure_ascii=True,
                ),
                flush=True,
            )

    def save(self):
        atomic_json(
            self.path,
            self.state,
        )

    def get(self, path):
        try:
            reply = self.api.request(
                'GET',
                path,
            )

        except TransportError:
            time.sleep(.5)
            return None

        if (
            reply.status >= 500
            or reply.status == 429
        ):
            time.sleep(.5)
            return None

        if reply.status not in (200, 403):
            raise MatchError(
                'unexpected GET response: '
                + str(reply.status)
            )

        if (
            reply.status == 403
            and (
                not isinstance(
                    reply.body,
                    dict,
                )
                or reply.body.get('code')
                != 'AccessTimeError'
            )
        ):
            raise MatchError(
                'unexpected forbidden response'
            )

        return reply

    def setting(self, new):
        if (
            not isinstance(new, dict)
            or not isinstance(
                new.get('agents'),
                list,
            )
        ):
            raise MatchError(
                'malformed setting'
            )

        old = self.state['setting']

        if old:
            a = copy.deepcopy(old)
            b = copy.deepcopy(new)

            sa = a.pop('startsAt')
            sb = b.pop('startsAt')

            if (
                a != b
                or (
                    sa != 0
                    and sa != sb
                )
            ):
                raise MatchError(
                    'match setting changed; '
                    'use a fresh run directory '
                    'for a new match'
                )

        n = len(new['agents'])

        kinds = self.kinds_option
        selection_diagnostics = None

        if kinds is None:
            if (
                self.supply_count is None
                or not (
                    0
                    <= self.supply_count
                    <= n
                )
            ):
                raise MatchError(
                    'supply-count outside agent count'
                )

            kinds, selection_diagnostics = (
                choose_supply_layout(
                    new,
                    self.supply_count,
                )
            )

        if (
            len(kinds) != n
            or any(
                type(k) is not int
                or k not in (0, 1)
                for k in kinds
            )
        ):
            raise MatchError(
                'kind array must match '
                'original agent order/count'
            )

        first_selection = (
            self.state['kinds'] is None
        )

        if (
            self.state['kinds'] is not None
            and self.state['kinds'] != kinds
        ):
            raise MatchError(
                'cannot change kinds '
                'in an existing match journal'
            )

        self.state.update(
            setting=copy.deepcopy(new),
            kinds=list(kinds),
        )

        self.save()

        if (
            first_selection
            and selection_diagnostics is not None
        ):
            self.emit(
                'supply_layout_selected',
                **selection_diagnostics,
                kinds=list(kinds),
            )

    def startup(self):
        deadline = (
            time.monotonic()
            + self.startup_timeout
        )

        while time.monotonic() < deadline:
            config = self.get('/setting')

            if (
                config
                and config.status == 403
                and self.state['days']
            ):
                last = (
                    self.state['days'][-1]
                    ['state']
                )

                if (
                    last['day']
                    == len(
                        self.state['setting']
                        ['daySteps']
                    ) - 1
                    and config.server_upper
                    >= last['endsAt']
                ):
                    return config

            if (
                config
                and config.status == 200
            ):
                self.setting(
                    config.body
                )

                if (
                    not self.state[
                        'type_accepted'
                    ]
                    and not self.state['days']
                ):
                    try:
                        result = self.api.request(
                            'POST',
                            '/agent',
                            self.state['kinds'],
                        )

                        if result.status == 200:
                            self.state[
                                'type_accepted'
                            ] = True

                            self.save()

                            self.emit(
                                'kinds_accepted',
                                kinds=self.state[
                                    'kinds'
                                ],
                            )

                    except TransportError:
                        # Repeating identical kinds is
                        # safe before their cutoff.
                        pass

                current = self.get('/')

                if (
                    current
                    and current.status == 200
                ):
                    if (
                        self.state['setting']
                        ['startsAt']
                        == 0
                    ):
                        sealed = self.get(
                            '/setting'
                        )

                        if (
                            not sealed
                            or sealed.status != 200
                            or not sealed.body.get(
                                'startsAt'
                            )
                        ):
                            continue

                        self.setting(
                            sealed.body
                        )

                    return current

            time.sleep(.5)

        raise MatchError(
            'no current match before startup timeout; '
            'journal preserved'
        )

    def propose(
        self,
        record,
        bundle,
        deadline,
        label,
    ):
        if (
            record.get(
                'ambiguous_payload'
            ) is not None
            and record[
                'ambiguous_payload'
            ] != bundle['answer']
        ):
            raise MatchError(
                'different payload forbidden '
                'after an ambiguous POST'
            )

        last = acknowledged(record)

        if (
            last
            and last['answer']
            == bundle['answer']
            and record.get(
                'ambiguous_payload'
            ) is None
        ):
            return True

        for retry in range(3):
            if (
                time.monotonic() + 2.5
                >= deadline
            ):
                return False

            try:
                fresh = self.api.request(
                    'GET',
                    '/',
                    deadline=deadline - 1.,
                )

                if fresh.status != 200:
                    return False

                if any(
                    fresh.body.get(k)
                    != record['state'][k]
                    for k in (
                        'day',
                        'endsAt',
                        'agents',
                        'traffics',
                    )
                ):
                    return False

                deadline = min(
                    deadline,
                    fresh.deadline(
                        record['state']
                        ['endsAt']
                    ),
                )

            except (
                TransportError,
                WindowClosed,
            ):
                continue

            attempt = dict(
                answer=bundle['answer'],
                report=bundle['report'],
                status='intent',
                label=label,
                request_started=time.time(),
                engine_seconds=bundle[
                    'engine_seconds'
                ],
            )

            record['attempts'].append(
                attempt
            )

            # Durable write-ahead intent before any
            # possible server acceptance.
            self.save()

            try:
                reply = self.api.request(
                    'POST',
                    '/',
                    bundle['answer'],
                    deadline=deadline - 1.,
                )

            except WindowClosed:
                attempt[
                    'status'
                ] = 'not_sent'

                self.save()
                return False

            except TransportError:
                attempt[
                    'status'
                ] = 'unknown'

                record[
                    'ambiguous_payload'
                ] = bundle['answer']

                self.save()
                continue

            revision = (
                reply.body.get('revision')
                if isinstance(
                    reply.body,
                    dict,
                )
                else None
            )

            if (
                reply.status == 200
                and type(revision) is int
            ):
                attempt['status'] = (
                    'accepted'
                    if revision >= 0
                    else 'rejected'
                )

                attempt[
                    'revision'
                ] = revision

            else:
                # Non-contract failures cannot prove
                # whether a POST was processed.
                attempt[
                    'status'
                ] = 'unknown'

                record[
                    'ambiguous_payload'
                ] = bundle['answer']

            attempt.update(
                response_status=reply.status,
                response_received=time.time(),
                conservative_margin=(
                    deadline
                    - time.monotonic()
                ),
                receipt_margin=(
                    record['state']
                    ['endsAt']
                    - reply.server_upper
                ),
            )

            self.save()

            self.emit(
                'answer_' + attempt['status'],
                day=record['state']['day'],
                label=label,
                revision=revision,
                objective=bundle[
                    'report'
                ]['objective'],
                margin=attempt[
                    'conservative_margin'
                ],
                retry=retry,
            )

            if (
                attempt['status']
                == 'accepted'
            ):
                if (
                    last
                    and revision
                    <= last['revision']
                ):
                    raise MatchError(
                        'non-monotonic receipt revision; '
                        'cannot determine last accepted plan'
                    )

                return True

            if (
                attempt['status']
                == 'rejected'
            ):
                return False

        return False

    def day(self, reply):
        current = reply.body
        setting = self.state['setting']

        d = current.get('day')

        if (
            type(d) is not int
            or not (
                0
                <= d
                < len(
                    setting['daySteps']
                )
            )
        ):
            raise MatchError(
                'invalid current day'
            )

        if (
            [
                a['kind']
                for a in current['agents']
            ]
            != self.state['kinds']
        ):
            raise MatchError(
                'server agent kinds '
                'differ from requested kinds'
            )

        deadline = reply.deadline(
            current['endsAt']
        )

        records = self.state['days']

        if (
            not records
            or d
            > records[-1]['state']['day']
        ):
            if (
                not records
                and d != 0
            ):
                raise MatchError(
                    'cannot start mid-match '
                    'without acknowledged earlier history'
                )

            if records:
                self.state['history'] = (
                    confirm_next(
                        setting,
                        records[-1],
                        current,
                    )
                )

                records[-1][
                    'next_state_verified'
                ] = True

                self.emit(
                    'day_confirmed',
                    day=d - 1,
                    evidence=(
                        'receipt + day closure + '
                        'next state + exclusive writer'
                    ),
                )

            record = dict(
                state=current,
                attempts=[],
                ambiguous_payload=None,
                solver_finished=False,
                next_state_verified=False,
                acquired_at=time.time(),
            )

            records.append(record)

            self.state[
                'status'
            ] = 'running'

            self.save()

        else:
            record = records[-1]

            if any(
                current.get(k)
                != record['state'][k]
                for k in (
                    'day',
                    'endsAt',
                    'agents',
                    'traffics',
                )
            ):
                raise MatchError(
                    'current daily state '
                    'changed unexpectedly'
                )

        pending = [
            a
            for a in record['attempts']
            if a['status']
            in ('intent', 'unknown')
        ]

        best = acknowledged(record)

        unresolved = (
            pending
            and (
                best is None
                or any(
                    a['answer']
                    != best['answer']
                    for a in pending
                )
            )
        )

        if unresolved:
            if (
                deadline
                - time.monotonic()
                < 4.
            ):
                return

            latest = pending[-1]

            record[
                'ambiguous_payload'
            ] = latest['answer']

            bundle = self.engine.evaluate(
                setting,
                current,
                self.state['history'],
                answer=latest['answer'],
                timeout=max(
                    .01,
                    min(
                        3.,
                        deadline
                        - time.monotonic()
                        - 2.,
                    ),
                ),
            )

            self.propose(
                record,
                bundle,
                deadline,
                'recover_identical',
            )

            record[
                'solver_finished'
            ] = True

            self.save()
            return

        if not acknowledged(record):
            waiting = [
                [
                    -setting[
                        'daySteps'
                    ][d]
                ]
                for _ in current['agents']
            ]

            try:
                bundle = self.engine.evaluate(
                    setting,
                    current,
                    self.state['history'],
                    answer=waiting,
                    timeout=max(
                        .01,
                        min(
                            3.,
                            deadline
                            - time.monotonic()
                            - 2.,
                        ),
                    ),
                )

            except (
                EngineError,
                subprocess.TimeoutExpired,
                OSError,
            ) as e:
                raise MatchError(
                    'cannot establish '
                    'replay-validated fallback: '
                    + str(e)
                ) from e

            if not self.propose(
                record,
                bundle,
                deadline,
                'fallback',
            ):
                return

        if (
            record['solver_finished']
            or record[
                'ambiguous_payload'
            ] is not None
        ):
            return

        budget = min(
            55.,
            deadline
            - time.monotonic()
            - self.reserve,
        )

        if budget < 1.:
            record[
                'solver_finished'
            ] = True

            self.save()

            self.emit(
                'solver_skipped',
                day=d,
                reason=(
                    'fallback protected; '
                    'near deadline'
                ),
            )

            return

        self.emit(
            'solver_started',
            day=d,
            budget=budget,
        )

        started = time.monotonic()

        try:
            proposed = self.engine.evaluate(
                setting,
                current,
                self.state['history'],
                budget=budget,
                timeout=min(
                    budget + .5,
                    deadline
                    - time.monotonic()
                    - 3.,
                ),
            )

            record[
                'solver_seconds'
            ] = (
                time.monotonic()
                - started
            )

            if (
                tuple(
                    proposed['report']
                    ['objective']
                )
                >= tuple(
                    acknowledged(record)
                    ['report']
                    ['objective']
                )
            ):
                # Separate output validation:
                # a crashed/truncated child cannot
                # bypass replay.
                verified = self.engine.evaluate(
                    setting,
                    current,
                    self.state['history'],
                    answer=proposed['answer'],
                    timeout=max(
                        .01,
                        min(
                            3.,
                            deadline
                            - time.monotonic()
                            - 2.,
                        ),
                    ),
                )

                if (
                    verified['report']['replay']
                    != proposed['report']['replay']
                ):
                    raise EngineError(
                        'solve/standalone validation '
                        'replay mismatch'
                    )

                verified[
                    'engine_seconds'
                ] += proposed[
                    'engine_seconds'
                ]

                self.propose(
                    record,
                    verified,
                    deadline,
                    'solver',
                )

        except (
            EngineError,
            subprocess.TimeoutExpired,
            OSError,
        ) as e:
            self.emit(
                'solver_failure_fallback_kept',
                day=d,
                error=str(e),
            )

            record[
                'solver_error'
            ] = str(e)

        record[
            'solver_finished'
        ] = True

        self.save()

    def finish(self):
        if (
            not self.state['days']
            or len(self.state['days'])
            != len(
                self.state['setting']
                ['daySteps']
            )
        ):
            raise MatchError(
                'match closed with missing days'
            )

        last = certain_plan(
            self.state['days'][-1]
        )

        self.state[
            'status'
        ] = 'completed'

        self.state[
            'final_state_independently_verified'
        ] = False

        self.state[
            'final_history_draft'
        ] = copy.deepcopy(
            last['report']
            ['next_history_draft']
        )

        self.save()

        summary = self.summary()

        atomic_json(
            self.root / 'summary.json',
            summary,
        )

        self.emit(
            'match_completed',
            predicted_score=summary[
                'predicted_score'
            ],
            note=(
                'No final score/state readback endpoint; '
                'score is local replay, '
                'not server certified.'
            ),
        )

        return summary

    def summary(self):
        rows = []

        for r in self.state['days']:
            a = certain_plan(r)

            rows.append(
                dict(
                    day=r['state']['day'],
                    revision=a['revision'],
                    objective=a[
                        'report'
                    ]['objective'],
                    solver_seconds=r.get(
                        'solver_seconds',
                        0.,
                    ),
                    accepted=len([
                        x
                        for x in r['attempts']
                        if x['status']
                        == 'accepted'
                    ]),
                    rejected=len([
                        x
                        for x in r['attempts']
                        if x['status']
                        == 'rejected'
                    ]),
                    unknown=len([
                        x
                        for x in r['attempts']
                        if x['status']
                        in (
                            'intent',
                            'unknown',
                        )
                    ]),
                    answer_seconds=(
                        a.get(
                            'response_received',
                            0,
                        )
                        - r['acquired_at']
                    ),
                    margin=a.get(
                        'conservative_margin'
                    ),
                    valid=a[
                        'report'
                    ]['replay']['valid'],
                    next_state_verified=r[
                        'next_state_verified'
                    ],
                    confirmed_history_days=(
                        r['state']['day'] + 1
                        if r[
                            'next_state_verified'
                        ]
                        else None
                    ),
                )
            )

        return dict(
            status=self.state['status'],
            days=rows,
            predicted_score=[
                sum(
                    row['objective'][i]
                    for row in rows
                )
                for i in range(3)
            ],
            server_score_available=False,
            final_state_independently_verified=False,
            history_evidence=(
                'single writer, durable revisions, '
                'sequential day closure '
                'and next-state comparison'
            ),
        )

    def run(self):
        key = digest(
            self.identity
        )

        with (
            Lock(self.root / 'run.lock'),
            Lock(
                team_lock_dir()
                / (key + '.lock')
            ),
        ):
            if self.path.exists():
                self.state = json.loads(
                    self.path.read_text(
                        encoding='utf-8'
                    )
                )

                if (
                    self.state.get(
                        'identity'
                    )
                    != self.identity
                    or self.state.get(
                        'format'
                    )
                    != 'c_staged.match_journal.v1'
                ):
                    raise MatchError(
                        'journal identity changed '
                        'before lock acquisition'
                    )

            if (
                self.state['status']
                == 'completed'
            ):
                return self.summary()

            first = self.startup()

            if first.status == 403:
                return self.finish()

            self.day(first)

            last_deadline = first.deadline(
                first.body['endsAt']
            )

            while True:
                time.sleep(.5)

                reply = self.get('/')

                if reply is None:
                    if (
                        time.monotonic()
                        > last_deadline + 30
                    ):
                        raise MatchError(
                            'connection unavailable '
                            'beyond day rollover; '
                            'journal left unconfirmed'
                        )

                    continue

                if reply.status == 200:
                    self.day(reply)

                    last_deadline = (
                        reply.deadline(
                            reply.body[
                                'endsAt'
                            ]
                        )
                    )

                    continue

                latest = (
                    self.state['days'][-1]
                )

                if (
                    reply.server_upper
                    >= latest[
                        'state'
                    ]['endsAt']
                    and latest[
                        'state'
                    ]['day']
                    == len(
                        self.state[
                            'setting'
                        ]['daySteps']
                    ) - 1
                ):
                    return self.finish()

                if (
                    reply.server_upper
                    > latest[
                        'state'
                    ]['endsAt'] + 10
                ):
                    raise MatchError(
                        'daily state unavailable '
                        'past expected rollover; '
                        'history left unconfirmed'
                    )


def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__
    )

    p.add_argument(
        '--base-url',
        required=True,
    )

    p.add_argument(
        '--allow-remote',
        action='store_true',
    )

    p.add_argument(
        '--token-env',
        default='PROCON_TOKEN',
    )

    p.add_argument(
        '--run-dir',
        required=True,
    )

    p.add_argument(
        '--engine',
        type=Path,
        default=DEFAULT_ENGINE,
    )

    group = (
        p.add_mutually_exclusive_group(
            required=True
        )
    )

    group.add_argument(
        '--kinds',
        help=(
            'explicit original-order '
            'comma separated 0/1 values'
        ),
    )

    group.add_argument(
        '--supply-count',
        type=int,
        help=(
            'choose exactly N supplies; '
            'their Agent IDs are selected '
            'automatically from initial positions'
        ),
    )

    p.add_argument(
        '--startup-timeout',
        type=float,
        default=600.,
    )

    args = p.parse_args(argv)

    try:
        token = os.environ.get(
            args.token_env
        )

        if not token:
            raise MatchError(
                'token environment variable '
                'is not set'
            )

        if (
            not math.isfinite(
                args.startup_timeout
            )
            or args.startup_timeout <= 0
        ):
            raise MatchError(
                'invalid startup timeout'
            )

        base = endpoint(
            args.base_url,
            args.allow_remote,
        )

        kinds = (
            [
                int(x)
                for x
                in args.kinds.split(',')
            ]
            if args.kinds is not None
            else None
        )

        runner = MatchRunner(
            base,
            token,
            args.run_dir,
            kinds=kinds,
            supply_count=(
                args.supply_count
            ),
            engine=Engine(
                args.engine,
                Path(
                    args.run_dir
                ).resolve() / 'jobs',
                (
                    args.token_env,
                ),
            ),
            startup_timeout=(
                args.startup_timeout
            ),
        )

        result = runner.run()

        print(
            json.dumps(result),
            flush=True,
        )

        return 0

    except (
        MatchError,
        ValueError,
        OSError,
        subprocess.TimeoutExpired,
    ) as e:
        print(
            json.dumps(
                dict(
                    error=str(e),
                    note=(
                        'Journal preserved; '
                        'no guessed history.'
                    ),
                )
            ),
            file=sys.stderr,
        )

        return 1

    except KeyboardInterrupt:
        print(
            'Interrupted; journal preserved. '
            'Restart with the same run directory.',
            file=sys.stderr,
        )

        return 130


if __name__ == '__main__':
    raise SystemExit(main())