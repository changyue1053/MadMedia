"""Two incompatible local control protocols; test-only, no external network."""
import argparse,json
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
p=argparse.ArgumentParser();p.add_argument('--port',type=int);p.add_argument('--root');a=p.parse_args()
class Handler(BaseHTTPRequestHandler):
 def log_message(self,*args): pass
 def reply(self,status,body):
  data=body.encode();self.send_response(status);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
 def do_GET(self): self.reply(200,'{}')
 def do_POST(self):
  body=self.rfile.read(int(self.headers.get('Content-Length','0'))).decode()
  if self.headers.get('Authorization')!='Bearer test-only-fixture':return self.reply(401,'denied')
  if self.path=='/json/objects':
   try: valid=json.loads(body).get('operation')=='store'
   except ValueError: valid=False
   return self.reply(201,'{"object":"json-object"}' if valid else '{}')
  if self.path=='/text/store':return self.reply(202,'STORED text-object' if body.startswith('store:') else 'bad')
  if self.path=='/bad':return self.reply(200,'not-json')
  self.reply(404,'missing')
ThreadingHTTPServer(('127.0.0.1',a.port),Handler).serve_forever()
