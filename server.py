"""Safety Homes Rev2 LAN server. Optional cloud/PDF dependencies: requirements.txt."""
import argparse
from contextlib import contextmanager
import io
import datetime as dt
import hashlib
import hmac
import http.server
import json
import math
import os
from pathlib import Path
import secrets
import sqlite3
import threading
import time
import urllib.request
import urllib.parse

ROOT = Path(__file__).resolve().parent
COMMANDS = {'AUTO', 'MAINTENANCE', 'SLEEP', 'WAKE', 'MANUAL_READ', 'ACTUATOR_TOGGLE', 'FAN_TOGGLE'}

class Store:
    def __init__(self, path):
        self.path = str(path)
        self.lock = threading.RLock()
        self.latest = {'temperature': None, 'humidity': None, 'distance': None, 'smoke': 'Waiting...', 'actuator': 'OFF', 'fan': 'Unknown', 'mode': 'UNKNOWN', 'status': 'WAITING', 'changed_at': None}
        self.telemetry_changed = threading.Condition(self.lock)
        self.last_seen = 0
        self.last_bridge_seen = 0
        self.cloud_error = ''
        self.cloud_success = None
        with self.db() as c:
            c.executescript('''
            CREATE TABLE IF NOT EXISTS packets(id TEXT PRIMARY KEY, captured REAL, received REAL, payload TEXT, synced INTEGER DEFAULT 0);
            CREATE TABLE IF NOT EXISTS readings(id TEXT PRIMARY KEY, captured REAL, temperature REAL, humidity REAL, distance REAL, smoke TEXT, raw INTEGER, quality TEXT, mode TEXT NOT NULL DEFAULT 'AUTO');
            CREATE TABLE IF NOT EXISTS sleeps(id TEXT PRIMARY KEY, start REAL, wake REAL, duration REAL, boot TEXT, tick INTEGER, quality TEXT);
            CREATE TABLE IF NOT EXISTS commands(id INTEGER PRIMARY KEY AUTOINCREMENT, command TEXT, created REAL, state TEXT DEFAULT 'PENDING', result TEXT);
            CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
            CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY, start REAL, stop REAL, observed REAL DEFAULT 0);
            ''')
            if 'mode' not in {column[1] for column in c.execute('PRAGMA table_info(readings)')}:
                c.execute("ALTER TABLE readings ADD COLUMN mode TEXT NOT NULL DEFAULT 'AUTO'")
            row = c.execute("SELECT value FROM meta WHERE key='latest'").fetchone()
            if row: self.latest.update(json.loads(row[0]))
    @contextmanager
    def db(self):
        c = sqlite3.connect(self.path, timeout=10)
        try:
            c.execute('PRAGMA journal_mode=WAL')
            c.execute('PRAGMA synchronous=FULL')
            yield c
            c.commit()
        except Exception:
            c.rollback()
            raise
        finally:
            c.close()
    def ingest(self, p):
        if not isinstance(p, dict) or not isinstance(p.get('id'), str) or not 1 <= len(p['id']) <= 120 or not isinstance(p.get('data'), dict):
            raise ValueError('Invalid packet envelope')
        d = p['data']
        if d.get('mode') not in ('AUTO', 'MAINTENANCE', 'SLEEP'): raise ValueError('Invalid mode')
        for key in ('temperature', 'humidity', 'distance'):
            v = d.get(key)
            if v is not None and (isinstance(v, bool) or not isinstance(v, (float, int)) or not math.isfinite(v)): raise ValueError('Invalid sensor value')
        now = time.time()
        captured = p.get('epoch', 0)
        quality = 'device_clock' if isinstance(captured, (int, float)) and math.isfinite(captured) and 1700000000 < captured < now + 300 else 'receipt_time'
        captured = captured if quality == 'device_clock' else now
        if quality == 'device_clock' and d.get('rtc_epoch') == captured:
            quality = 'rtc_clock'
        boot = str(p.get('boot', ''))
        tick = int(p.get('tick', 0))
        if not 0 <= tick <= 0xffffffff: raise ValueError('Invalid uptime')
        persistent = p.get('durable') is True
        with self.lock, self.db() as c:
            if persistent:
                inserted = c.execute('INSERT OR IGNORE INTO packets VALUES(?,?,?,?,0)', (p['id'], captured, now, json.dumps(p, allow_nan=False))).rowcount
                if not inserted: return
            mode = d['mode']
            kind = d.get('kind')
            manual_reading = mode == 'MAINTENANCE' and kind == 'manual_reading'
            saved_reading = persistent and p.get('log') is True and ((mode == 'AUTO' and kind == 'reading') or manual_reading)
            if saved_reading:
                reading_time, reading_quality = captured, quality
                if manual_reading and d.get('ack'):
                    command_row = c.execute(
                        "SELECT created FROM commands WHERE id=? AND command='MANUAL_READ'",
                        (int(d['ack']),),
                    ).fetchone()
                    if command_row:
                        reading_time, reading_quality = command_row[0], 'button_press'
                c.execute('INSERT OR IGNORE INTO readings VALUES(?,?,?,?,?,?,?,?,?)',
                          (p['id'], reading_time, d.get('temperature'), d.get('humidity'),
                           d.get('distance'), d.get('smoke'), d.get('smoke_raw'), reading_quality, mode))
            if persistent and d.get('kind') == 'mode':
                opened = c.execute('SELECT id,start,boot,tick,quality FROM sleeps WHERE wake IS NULL ORDER BY start DESC LIMIT 1').fetchone()
                if d['mode'] == 'SLEEP' and not opened:
                    c.execute('INSERT INTO sleeps VALUES(?,?,NULL,NULL,?,?,?)', (p['id'], captured, boot, tick, quality))
                elif d['mode'] != 'SLEEP' and opened:
                    duration = ((tick - opened[3]) & 0xffffffff) / 1000 if boot == opened[2] else None
                    q = ('rtc_start_bridge_duration' if opened[4] == 'rtc_clock' else opened[4]) if duration is not None else 'interrupted_by_esp_restart'
                    wake = opened[1] + duration if duration is not None else captured
                    if opened[4] == 'rtc_clock' and quality == 'rtc_clock' and captured >= opened[1] and d.get('message') in ('hardware_wake', 'wake_line_low'):
                        wake = captured
                        duration = wake - opened[1]
                        q = 'rtc_clock'
                    c.execute('UPDATE sleeps SET wake=?,duration=?,quality=? WHERE id=?', (wake, duration, q, opened[0]))
            if d.get('ack'):
                manual_saved = manual_reading and saved_reading
                ok = bool(d.get('ok'))
                c.execute(
                    """UPDATE commands SET state=?,result=? WHERE id=?
                       AND (command != 'MANUAL_READ' OR ? OR NOT ?)
                       AND (state IN ('PENDING','SENT')
                            OR (state='EXPIRED' AND command='MANUAL_READ' AND ?))""",
                    ('ACKED' if ok else 'REJECTED',
                     'reading_saved' if manual_saved else d.get('message', ''),
                     int(d['ack']), manual_saved, ok, manual_saved),
                )
            if not p.get('backlog'):
                old_mode = self.latest['mode']
                self.latest.update(d)
                if old_mode != d['mode']: self.latest['changed_at'] = captured
                self.latest['status'] = 'ASLEEP' if d['mode'] == 'SLEEP' else 'AWAKE'
                self.latest['time_quality'] = quality
                for key in ('storage_ok','dropped','storage_errors','queued_records','mcu_age_ms','recovered','uart_errors'):
                    if key in p: self.latest[key] = p[key]
                self.last_seen = now
                c.execute("INSERT OR REPLACE INTO meta VALUES('latest',?)", (json.dumps(self.latest),))
                self.telemetry_changed.notify_all()

    def wait_for_telemetry(self, after):
        with self.telemetry_changed:
            self.telemetry_changed.wait_for(lambda: self.last_seen != after, timeout=2)

    def command(self):
        with self.lock, self.db() as c:
            c.execute("UPDATE commands SET state='EXPIRED',result='Device did not acknowledge in 30 seconds' WHERE state IN ('PENDING','SENT') AND created < ?", (time.time()-30,))
            row = c.execute("SELECT id,command FROM commands WHERE state IN ('PENDING','SENT') ORDER BY created LIMIT 1").fetchone()
            if row: c.execute("UPDATE commands SET state='SENT' WHERE id=?", (row[0],))
            return row
    def queue(self, cmd):
        if cmd not in COMMANDS: raise ValueError('Unknown command')
        with self.lock, self.db() as c:
            c.execute("UPDATE commands SET state='EXPIRED' WHERE state IN ('PENDING','SENT') AND created < ?", (time.time()-30,))
            if c.execute("SELECT COUNT(*) FROM commands WHERE state IN ('PENDING','SENT')").fetchone()[0] >= 8: raise ValueError('Command queue is full')
            if self.latest['mode']=='SLEEP' and cmd!='WAKE': raise ValueError('Wake the MCU first')
            if cmd in ('MANUAL_READ','ACTUATOR_TOGGLE','FAN_TOGGLE') and self.latest['mode']!='MAINTENANCE': raise ValueError('Enter MAINTENANCE first')
            # Random IDs prevent stale ESP acknowledgments matching a new DB's command 1.
            while True:
                ident=secrets.randbelow(0x7ffffffe)+1
                if not c.execute('SELECT 1 FROM commands WHERE id=?',(ident,)).fetchone(): break
            c.execute('INSERT INTO commands(id,command,created) VALUES(?,?,?)', (ident,cmd,time.time()))
            return ident
    def data(self):
        with self.lock, self.db() as c:
            c.execute("UPDATE commands SET state='EXPIRED',result='No device acknowledgment within 30 seconds' WHERE state IN ('PENDING','SENT') AND created < ?", (time.time()-30,))
            out = dict(self.latest)
            now = time.time()
            out['online'] = now-max(self.last_seen,self.last_bridge_seen) < 15
            out['last_bridge_poll'] = self.last_bridge_seen or None
            out['last_seen'] = self.last_seen or None
            age_ms = out.get('mcu_age_ms')
            fresh = bool(self.last_seen and isinstance(age_ms, (int, float))
                         and 0 <= age_ms < 15000
                         and age_ms + max(0, now-self.last_seen)*1000 < 15000)
            out['mcu_online'] = out['online'] and (out['mode']=='SLEEP' or fresh)
            out['readings_available'] = fresh and out['mode'] in ('AUTO', 'MAINTENANCE')
            out['cloud_configured'] = bool(os.getenv('FIREBASE_URL'))
            out['cloud_error'] = self.cloud_error
            out['cloud_last_success'] = self.cloud_success
            out['sync_pending'] = c.execute('SELECT COUNT(*) FROM packets WHERE synced=0').fetchone()[0]
            out['commands'] = [dict(zip(('id','command','state','result'), r)) for r in c.execute('SELECT id,command,state,result FROM commands ORDER BY created DESC LIMIT 8')]
            return out
    def history(self, sleep=False):
        def stamp(t): return dt.datetime.fromtimestamp(t).astimezone().isoformat(timespec='seconds') if t else 'In progress'
        with self.lock, self.db() as c:
            if sleep:
                return [[r[0], stamp(r[1]), stamp(r[2]), f'{r[3]:.1f}s' if r[3] is not None else r[4]] for r in c.execute('SELECT id,start,wake,duration,quality FROM sleeps ORDER BY start DESC, rowid DESC LIMIT 2000').fetchall()[::-1]]
            return [[r[0], stamp(r[1]), r[2], r[3], r[4], r[5], r[6], r[7], r[8]] for r in c.execute('SELECT id,captured,temperature,distance,smoke,humidity,raw,quality,mode FROM readings ORDER BY captured DESC, rowid DESC LIMIT 2000').fetchall()[::-1]]

    def delete_readings(self, ids):
        if not isinstance(ids, list) or not 1 <= len(ids) <= 100 or any(
            not isinstance(item, str) or not 1 <= len(item) <= 120 for item in ids
        ):
            raise ValueError('Select between 1 and 100 valid reading IDs')
        placeholders = ','.join('?' for _ in ids)
        with self.lock, self.db() as c:
            deleted = c.execute(f'DELETE FROM readings WHERE id IN ({placeholders})', ids).rowcount
        return {'readings': deleted}

    def clear_history(self, target=None):
        if target not in (None, 'sleep'):
            raise ValueError('Unknown record type')
        with self.lock, self.db() as c:
            cleared_sleeps = c.execute('DELETE FROM sleeps WHERE wake IS NOT NULL').rowcount
            if target == 'sleep':
                return {'sleeps': cleared_sleeps}
            return {
                'readings': c.execute('DELETE FROM readings').rowcount,
                'sleeps': cleared_sleeps,
                'sessions': 0,
                'commands': c.execute("DELETE FROM commands WHERE state NOT IN ('PENDING','SENT')").rowcount,
            }

    def sessions(self):
        with self.db() as c:
            return [dict(zip(('id','start','stop','observed_auto_seconds'),r)) for r in c.execute('SELECT id,start,stop,observed FROM sessions ORDER BY start DESC')]
    def session_action(self, action):
        now=time.time()
        with self.lock, self.db() as c:
            running=c.execute('SELECT id,start,observed FROM sessions WHERE stop IS NULL LIMIT 1').fetchone()
            if action=='start':
                if running: raise ValueError('A session is already running')
                ident=secrets.token_hex(12);c.execute('INSERT INTO sessions(id,start) VALUES(?,?)',(ident,now))
            elif action=='stop':
                if not running: raise ValueError('No session is running')
                c.execute('UPDATE sessions SET stop=? WHERE id=?',(now,running[0]))
                payload={'type':'testing_session','id':running[0],'start':running[1],'stop':now,'observed_auto_seconds':running[2], 'note':'Wall time alone does not prove hardware operation; observed time excludes server outages.'}
                c.execute('INSERT INTO packets VALUES(?,?,?,?,0)',('session-'+running[0],now,now,json.dumps(payload)))
            else: raise ValueError('Unknown session action')
        return {'ok':True}
    def observe_session(self, seconds):
        with self.lock:
            active=time.time()-self.last_seen<15 and self.latest.get('mcu_age_ms',999999)<15000 and self.latest['mode']=='AUTO'
            if active:
                with self.db() as c: c.execute('UPDATE sessions SET observed=observed+? WHERE stop IS NULL',(max(0,min(seconds,5)),))


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = 'SafetyHomes/Rev2'
    def reply(self, value, code=200, cookie=None, plain=False):
        body = (str(value) if plain else json.dumps(value, allow_nan=False)).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'text/plain' if plain else 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control','no-store')
        if cookie: self.send_header('Set-Cookie', cookie)
        self.end_headers(); self.wfile.write(body)
    def logged_in(self):
        cookies = dict(x.strip().split('=',1) for x in self.headers.get('Cookie','').split(';') if '=' in x)
        return self.server.sessions.get(cookies.get('session'), 0) > time.time()
    def device(self): return hmac.compare_digest(self.headers.get('X-Device-Key',''), self.server.device_key)
    def body(self):
        n = int(self.headers.get('Content-Length','0'))
        if not 0 < n <= 16384: raise ValueError('Invalid request size')
        return json.loads(self.rfile.read(n))
    def do_GET(self):
        if self.path == '/':
            body = (ROOT/'index.html').read_bytes()
            self.send_response(200); self.send_header('Content-Type','text/html; charset=utf-8'); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body); return
        sensor_images = {
            '/dht22.jpeg': ('dht22.jpeg', 'dht22.jpg'),
            '/hcsr04.jpg': ('hcsr04.jpg', 'hcsr04.jpeg'),
            '/mq2.jpg': ('mq2.jpg', 'mq2.jpeg'),
        }
        if self.path in sensor_images:
            for filename in sensor_images[self.path]:
                try:
                    body = (ROOT / filename).read_bytes()
                except FileNotFoundError:
                    continue
                self.send_response(200)
                self.send_header('Content-Type', 'image/jpeg')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self.send_error(404, 'Sensor image missing')
            return
        if self.path == '/api/get_command':
            if not self.device(): self.reply({'error':'Device key rejected'},403); return
            self.server.store.last_bridge_seen = time.time()
            row = self.server.store.command()
            self.reply(f'{int(time.time())}|{row[0] if row else 0}|{row[1] if row else "NONE"}', plain=True); return
        if not self.logged_in(): self.reply({'error':'Login required'},401); return
        path = urllib.parse.urlsplit(self.path)
        if path.path == '/api/data':
            after = urllib.parse.parse_qs(path.query).get('after', [])
            if after:
                try:
                    last_seen = float(after[0])
                except ValueError:
                    self.reply({'error': 'Invalid telemetry timestamp'}, 400)
                    return
                self.server.store.wait_for_telemetry(last_seen)
                if not self.logged_in():
                    self.reply({'error': 'Login required'}, 401)
                    return
            self.reply(self.server.store.data())
        elif self.path in ('/api/history','/api/sleep_history'): self.reply(self.server.store.history(self.path.endswith('sleep_history')))
        elif self.path == '/api/sessions': self.reply(self.server.store.sessions())
        elif self.path == '/api/report.pdf':
            try:
                body=make_report(self.server.store)
                self.send_response(200);self.send_header('Content-Type','application/pdf');self.send_header('Content-Length',str(len(body)));self.send_header('Content-Disposition','attachment; filename="safetyhomes_logs.pdf"');self.end_headers();self.wfile.write(body)
            except ImportError: self.reply({'error':'Install PDF support: python -m pip install reportlab'},503)
        else: self.reply({'error':'Not found'},404)
    def do_POST(self):
        try:
            p = self.body()
            if not isinstance(p, dict): raise ValueError('Expected JSON object')
            if self.path == '/api/login':
                with self.server.store.db() as c:
                    row=c.execute("SELECT value FROM meta WHERE key='credentials'").fetchone()
                if row:
                    cfg=json.loads(row[0])
                    digest=hashlib.pbkdf2_hmac('sha256',str(p.get('password','')).encode(),cfg['salt'].encode(),200000).hex()
                    valid=hmac.compare_digest(str(p.get('username','')),cfg['user']) and hmac.compare_digest(digest,cfg['hash'])
                else:
                    valid=hmac.compare_digest(str(p.get('username','')),self.server.username) and hmac.compare_digest(str(p.get('password','')),self.server.password)
                if not valid: self.reply({'error':'Invalid credentials'},401); return
                token=secrets.token_urlsafe(32); self.server.sessions[token]=time.time()+28800
                self.reply({'ok':True},cookie=f'session={token}; HttpOnly; SameSite=Strict; Path=/; Max-Age=28800'); return
            if self.path == '/api/update':
                if not self.device(): self.reply({'error':'Device key rejected'},403); return
                self.server.store.ingest(p); self.reply({'ok':True}); return
            if not self.logged_in(): self.reply({'error':'Login required'},401); return
            if self.path == '/api/command': self.reply({'id':self.server.store.queue(p.get('command'))},202)
            elif self.path == '/api/session': self.reply(self.server.store.session_action(p.get('action')))
            elif self.path == '/api/credentials':
                u,pw = str(p.get('username','')),str(p.get('password',''))
                if not u or len(pw)<8: raise ValueError('Username required; password needs at least 8 characters')
                # Persist only a salted hash; restart loads it through configure_credentials.
                salt = secrets.token_hex(16)
                digest = hashlib.pbkdf2_hmac('sha256',pw.encode(),salt.encode(),200000).hex()
                with self.server.store.db() as c: c.execute("INSERT OR REPLACE INTO meta VALUES('credentials',?)",(json.dumps({'user':u,'salt':salt,'hash':digest}),))
                self.server.username=u; self.server.password=pw
                self.reply({'ok':True})
            elif self.path == '/api/logout': self.server.sessions.clear(); self.reply({'ok':True},cookie='session=; Max-Age=0; Path=/')
            elif self.path == '/api/history/delete': self.reply(self.server.store.delete_readings(p.get('ids')))
            elif self.path == '/api/clear': self.reply(self.server.store.clear_history(p.get('target')))
            else: self.reply({'error':'Not found'},404)
        except (ValueError, TypeError, KeyError, OverflowError) as e: self.reply({'error':str(e)},400)

class Server(http.server.ThreadingHTTPServer):
    daemon_threads=True
    def __init__(self,address,store):
        super().__init__(address,Handler)
        self.store=store; self.sessions={}
        self.device_key=os.getenv('SAFETY_DEVICE_KEY','change-me-device-key')
        self.username=os.getenv('SAFETY_USER','admin'); self.password=os.getenv('SAFETY_PASSWORD','admin')

class CloudAuth:
    """Optional service-account OAuth refresh; no private key is ever served to clients."""
    def __init__(self, filename): self.filename=filename; self.credentials=None
    def token(self):
        if self.credentials is None:
            from google.oauth2 import service_account
            self.credentials=service_account.Credentials.from_service_account_file(self.filename,scopes=['https://www.googleapis.com/auth/firebase.database','https://www.googleapis.com/auth/userinfo.email'])
        if not self.credentials.valid:
            from google.auth.transport.requests import Request
            self.credentials.refresh(Request())
        return self.credentials.token

def sync_once(store,url,token='',oauth=None):
    if not url: return 0
    u=urllib.parse.urlsplit(url)
    if u.scheme!='https' or not u.hostname or not u.hostname.endswith(('.firebaseio.com','.firebasedatabase.app')) or u.path not in ('','/') or u.query or u.fragment:
        raise ValueError('Use the HTTPS Firebase Realtime Database base URL without /logs.json')
    with store.db() as c: rows=c.execute('SELECT id,payload FROM packets WHERE synced=0 ORDER BY received LIMIT 100').fetchall()
    count=0
    for ident,payload in rows:
        key=hashlib.sha256(ident.encode()).hexdigest()
        target=url.rstrip('/')+'/safetyhomes_rev2/packets/'+key+'.json'
        headers={'Content-Type':'application/json'}
        if oauth: headers['Authorization']='Bearer '+oauth.token()
        elif token: target+='?auth='+urllib.parse.quote(token,safe='')
        req=urllib.request.Request(target,data=payload.encode(),headers=headers,method='PUT')
        with urllib.request.urlopen(req,timeout=5) as res:
            if res.status != 200: raise RuntimeError('Firebase rejected write')
        with store.db() as c: c.execute('UPDATE packets SET synced=1 WHERE id=?',(ident,))
        count+=1
    return count

def make_report(store):
    from reportlab.platypus import SimpleDocTemplate,Paragraph,Spacer,LongTable,TableStyle,PageBreak
    from reportlab.lib.styles import getSampleStyleSheet
    from reportlab.lib import colors
    from reportlab.lib.styles import ParagraphStyle
    import html
    output=io.BytesIO();styles=getSampleStyleSheet();flow=[]
    cell=ParagraphStyle('Cell',parent=styles['BodyText'],fontSize=8,leading=10)
    def para(t): return Paragraph(html.escape(str(t)),styles['BodyText'])
    def table(headers,rows,widths):
        def item(x): return Paragraph(html.escape(str('Unavailable' if x is None else x)),cell)
        data=[[item(x) for x in headers]]+[[item(x) for x in r] for r in rows]
        t=LongTable(data,colWidths=widths,repeatRows=1,hAlign='LEFT');t.setStyle(TableStyle([('BACKGROUND',(0,0),(-1,0),colors.HexColor('#e5edf5')),('VALIGN',(0,0),(-1,-1),'TOP'),('GRID',(0,0),(-1,-1),0.3,colors.lightgrey),('LEFTPADDING',(0,0),(-1,-1),4),('RIGHTPADDING',(0,0),(-1,-1),4)]));return t
    flow += [Paragraph('Safety Homes - actual stored logs',styles['Title']),para('Generated '+dt.datetime.now().astimezone().isoformat(timespec='seconds')),para('RTC, bridge clock, and receipt-time labels identify timestamp quality. No test data is fabricated.'),Spacer(1,12)]
    rows=store.history();flow += [Paragraph('System readings',styles['Heading2']),table(['Timestamp','Temp C','Humidity %','Distance cm','Smoke','ADC','Time source','Mode'],[[r[1],r[2],r[5],r[3],r[4],r[6],r[7],r[8]] for r in rows],[125,45,55,60,65,35,75,80])]
    flow += [PageBreak(),Paragraph('Sleep periods',styles['Heading2']),para('Entry is firmware-reported; verify real power-down separately with current and wake measurements.'),Spacer(1,8),table(['Start','Wake','Seconds / status'],[[r[1],r[2],r[3]] for r in store.history(True)],[190,190,160])]
    flow += [Spacer(1,16),Paragraph('Testing sessions',styles['Heading2']),para('Observed AUTO time is only the time the PC saw a recently reporting AUTO MCU. It excludes unverified server-offline periods; wall time alone does not establish the required 36 hours.'),Spacer(1,8)]
    sessions=store.sessions()
    stamp=lambda x:dt.datetime.fromtimestamp(x).astimezone().isoformat(timespec='seconds') if x else 'Running'
    flow += [table(['Start','Stop','Observed AUTO hours'],[[stamp(r['start']),stamp(r['stop']),round(r['observed_auto_seconds']/3600,3)] for r in sessions],[190,190,160])]
    def footer(canvas,doc):canvas.setFont('Helvetica',8);canvas.drawString(36,22,'Safety Homes | Page '+str(doc.page))
    SimpleDocTemplate(output,pagesize=(612,936),leftMargin=36,rightMargin=36,topMargin=36,bottomMargin=36).build(flow,onFirstPage=footer,onLaterPages=footer)
    return output.getvalue()

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--port',type=int,default=8080);ap.add_argument('--db',default=str(ROOT/'safetyhomes_rev2.sqlite3'));args=ap.parse_args()
    store=Store(args.db);stop=threading.Event()
    account=os.getenv('FIREBASE_SERVICE_ACCOUNT');oauth=CloudAuth(account) if account else None
    def cloud_worker():
        while not stop.is_set():
            try:
                written=sync_once(store,os.getenv('FIREBASE_URL',''),os.getenv('FIREBASE_AUTH',''),oauth)
                if written:store.cloud_success=time.time();store.cloud_error=''
            except Exception as e:
                # Do not print URLs/tokens or private-key details.
                store.cloud_error='Cloud sync failed ('+type(e).__name__+'); retained for retry'
                print(store.cloud_error)
            stop.wait(30)
    def observer():
        previous=time.monotonic()
        while not stop.wait(5):
            now=time.monotonic();store.observe_session(now-previous);previous=now
    threading.Thread(target=cloud_worker,daemon=True).start();threading.Thread(target=observer,daemon=True).start()
    print(f'Open http://localhost:{args.port}. Default login admin / admin; change it in Settings. Database: {args.db}')
    print('Cloud:', 'configured' if os.getenv('FIREBASE_URL') else 'NOT configured; set FIREBASE_URL and credentials')
    try:
        with Server(('0.0.0.0',args.port),store) as server: server.serve_forever()
    except KeyboardInterrupt: pass
    finally: stop.set()
if __name__=='__main__': main()
