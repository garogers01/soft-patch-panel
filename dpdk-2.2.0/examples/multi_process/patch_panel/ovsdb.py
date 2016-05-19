import sys
import Queue
import socket
import json
from thread import *
from select import select
import getopt, time

OVSDB_IP = '127.0.0.1'
OVSDB_PORT = 6632
DEFAULT_DB = 'Patch_Panel'
BUFFER_SIZE = 4096

# TODO: Could start by getting the DB name and using that for ongoing requests
# TODO: How to keep an eye out for monitor, update, echo messages?
def gather_reply(socket):
    print "Waiting for reply"
    result = ""
    # we got the whole thing if we received all the fields
    sys.stdout.write('reply = ')
    while "error" not in result or "id" not in result or "result" not in result:
        reply = socket.recv(BUFFER_SIZE)
        print reply
        result += reply
    sys.stdout.write('result = ')	
    print result
    return json.loads(result)

def listen_for_messages(sock, message_queues, recv_data_queue):
    # To send something, add a message to queue and append sock to outputs
    sockname = sock.getsockname() 
    inputs = [sock]
    outputs = []
    while sock:
        readable, writable, exceptional = select(inputs, outputs, [])
        for s in readable:
            if s is sock:
                data = sock.recv(4096)
                # should test if its echo, if so, reply
                # message_type = get_msg_type(data)
                # if message_type is "echo":
                #   send_echo(message_
                message_queues[sock].put(data)
                outputs.append(sock)
                recv_data_queue.put("recv:" + str(sockname) + ":" + "{" + data + "}")
            #elif s is sys.stdin:
			#	continue
                #print sys.stdin.readline()
                #sock.close()
                #return
            else:
                print "error"
        for w in writable:
            if w is sock:
                sock.send(message_queues[sock].get_nowait())
                outputs.remove(sock)
            else:
                print "error"

def list_dbs():
	list_dbs_query =  {"method":"list_dbs", "params":[], "id": 0}
	print list_dbs_query
	return json.dumps(list_dbs_query)

def add_bridge():
    add_bridge_msg = {"method":"transact","params":["Open_vSwitch", {"row": {"bridges": ["named-uuid","new_bridge"]}, "table": "Open_vSwitch",
            "uuid-name": "new_switch","op": "insert"},{"row": {"name": "br1","type": "internal"},"table": "Interface","uuid-name": "new_interface", "op": "insert"},{"row": {"name": "br1","interfaces": ["named-uuid","new_interface"]},"table": "Port","uuid-name": "new_port","op": "insert"},{
            "row": {"name": "br1","ports": ["named-uuid","new_port"]},"table": "Bridge","uuid-name": "new_bridge","op": "insert"}],"id":0}
	
	#{"method": "transact", "id": 0, "params": ["Open_vSwitch", {"op": "select", "table": "Port", "where": [], "columns": ["interfaces", "_uuid", "name"]}]}
    print add_bridge_msg
    return json.dumps(add_bridge_msg)
	
	
def get_schema(socket, db_name = DEFAULT_DB, current_id = 0):
	list_schema = {"method": "get_schema", "params":[db_name], "id": current_id}
	print list_schema
	socket.send(json.dumps(list_schema))
	result = gather_reply(socket)
	return result
	
#    list_schema = {"method": "get_schema", "params":[db_name], "id": current_id}
#    socket.send(json.dumps(list_schema))
#    result = gather_reply(socket)
#    return result

def get_schema_version(socket, db = DEFAULT_DB):
    db_schema = get_schema(socket, db)
    return db_schema['version']

def list_tables(server, db):
    # keys that are under 'tables'
    db_schema = get_schema(socket, db)
    # return db_schema['tables'].keys
    return json.loads()


def list_columns(server, db):
    return

def monitor(columns, monitor_id = None, db = DEFAULT_DB):
	msg = {"method":"monitor", "params":[db, monitor_id, columns], "id":0}
	return json.dumps(msg)
	
def initdb (db=DEFAULT_DB):
	query = {
	"method":"transact", 
	"params": 
	[
		db,
		{
			"table":"Patch_Panel",
			"row":  {},
			"op":"insert"
		},
	],
	"id":0}
	print query
	return json.dumps(query)		

def resetdb (db=DEFAULT_DB):
	query = {
	"method":"transact", 
	"params": 
	[
		db,
		{
			"table":"Patch_Panel", 
			"where": [],
			"op":"delete"
		},
	],
	"id":0}
	print query
	return json.dumps(query)		
	
def addbr (br_name, db=DEFAULT_DB):
	query = {
	"method":"transact", 
	"params": 
	[
		db,
		{
			"table":"Bridge", 
			"row":
			{
				"name":br_name
			}, 
			"uuid-name": "newitem",
			"op":"insert"
		}, 
		{
			"table":"Patch_Panel",
			"where": [],			
			"mutations": [
				[
					"bridges",
					"insert",
					[
						"set",
						[
							[
								"named-uuid", 
								"newitem"
							]
						]
					]
				]
			],
			"op":"mutate"
		}],
		"id":0}
	print query
	return json.dumps(query)	

def addport (br_name, port_name, db=DEFAULT_DB):
	query = {
	"method":"transact", 
	"params": 
	[
		db,
		{
			"table":"Port", 
			"row":
			{
				"name":port_name
			}, 
			"uuid-name": "newitem",
			"op":"insert"
		}, 
		{
			"table":"Bridge",
			"where": [
				["name", "==", br_name]
			],			
			"mutations": [
				[
					"ports",
					"insert",
					[
						"set",
						[
							[
								"named-uuid", 
								"newitem"
							]
						]
					]
				]
			],
			"op":"mutate"
		}],
		"id":0}
	print query
	return json.dumps(query)	
	
def findbr (br_name, db=DEFAULT_DB):
	query = {
	"method":"transact", 
	"params": 
	[
		db,
		{
			"table":"Bridge", 
			"where": [
				["name", "==", br_name]
			],
			"op":"select"
		}		
	],
	"id":0}
	print query
	return json.dumps(query)	

def delbr (br_name, uuid, db=DEFAULT_DB):
	query = {
	"method":"transact", 
	"params": 
	[
		db,
		{
			"table":"Patch_Panel", 
			"where": [],
			"mutations": [
				[
					"bridges",
					"delete",
					[
						"set",
						[
							[
								"uuid",
								uuid
							]
						]
					]
				]
			],
			"op":"mutate"
		},
		{
			"table":"Bridge", 
			"where": [
				["name", "==", br_name]
			],
			"op":"delete"
		}		
	],
	"id":0}
	print query
	return json.dumps(query)	

def findport (port_name, db=DEFAULT_DB):
	query = {
	"method":"transact", 
	"params": 
	[
		db,
		{
			"table":"Port", 
			"where": [
				["name", "==", port_name]
			],
			"op":"select"
		}		
	],
	"id":0}
	print query
	return json.dumps(query)

def delport (br_name, port_name, uuid, db=DEFAULT_DB):
	query = {
	"method":"transact", 
	"params": 
	[
		db,
		{
			"table":"Bridge", 
			"where": [
				["name", "==", br_name]
			],
			"mutations": [
				[
					"ports",
					"delete",
					[
						"set",
						[
							[
								"uuid",
								uuid
							]
						]
					]
				]
			],
			"op":"mutate"
		},
		{
			"table":"Port", 
			"where": [
				["name", "==", port_name]
			],
			"op":"delete"
		}		
	],
	"id":0}
	print query
	return json.dumps(query)	
	
def insert (db_name, table, value):
	insert_cmd = [db_name,{"table":table, "row":{"name":value}, "op":"insert"}]
	print insert_cmd
	return insert_cmd

def transact(transactions):
    # Variants of this will add stuff
	jsonrpc = {"method":"transact", "params":transactions, "id":0}
	print jsonrpc
	return json.dumps(jsonrpc)

def monitor_cancel():
    return

def locking():
    return

def echo():
    echo_msg = {"method":"echo","id":"echo","params":[]}
    return json.dumps(echo_msg)

def dump(server, db):
    return

def list_bridges(db = DEFAULT_DB):
    # What if we replaced with a more specific query
    # columns = {"Bridge":{"name"}}
    #columns = {"Port":{"columns":["fake_bridge","interfaces","name","tag"]},"Controller":{"columns":[]},"Interface":{"columns":["name"]},"Open_vSwitch":{"columns":["bridges","cur_cfg"]},"Bridge":{"columns":["controller","fail_mode","name","ports"]}}
    #columns = {"Bridge":{"name"}}
    # TODO: cancel the monitor after we're done?
    return monitor(columns, db)

def monitorthread(s, message_queues, recv_data_queue):
	listen_for_messages(s, message_queues, recv_data_queue)
	
def main():
	global OVSDB_IP
	global OVSDB_PORT
	prompt = '$ '
	
	try:
		opts, args = getopt.getopt(sys.argv[1:],"s:p:h",["help","svr=","port="])
	except getopt.GetoptError:
		print 'ovs.py -p <port_number>'
		sys.exit(2)
	for opt, arg in opts:
		if opt == '-h':
			print 'ovsdb.py -s <ovsdb server> -p <port_number>'
			sys.exit()
		elif opt in ("-s", "--svr"):
			OVSDB_IP = arg
			print 'ovsdb server is ', OVSDB_IP		
		elif opt in ("-p", "--port"):
			OVSDB_PORT = int(arg)
			print 'port is ', OVSDB_PORT
		else:
			assert False, "unhandled option"			

	s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
	s.connect((OVSDB_IP, OVSDB_PORT))

	current_id = 0

	sockname = s.getsockname()
	print sockname
	print sockname[0]
	print sockname[1]
	
	#give a small delay
	time.sleep(0.1)	
	
	# initialization
	s.send(list_dbs())
	db_list = gather_reply(s)
	db_name = db_list['result'][0]
	print db_name
	print "-----"
	db_schema = get_schema(s, db_name)
	print db_schema['result']['version']
	tables = db_schema['result']['tables']
	print "-----"
	
	# message_queue is fifo for comm monitor thread <-> main
	message_queues = {}
	message_queues[s] = Queue.Queue()
	recv_data_queue = Queue.Queue()
	start_new_thread(monitorthread,(s,message_queues,recv_data_queue)) 
	
	#print "list bridges:"
	#s.send(list_bridges())
	#bridge_list = gather_reply(s)
	#print bridge_list	
	#bridges = bridge_list['result']['Bridge']
	#interfaces = bridge_list['result']['Interface']
	
	while True:
		try:
			
			while (recv_data_queue.qsize() > 0):
				print recv_data_queue.get_nowait()
			input = raw_input(prompt)
		except (KeyError, KeyboardInterrupt) as e:
			print "exit" 
			break	
		
		cmds = input.split (' ')	
		
#		if len (cmds) < 1:
#				continue
		if input == 'h' or input == '?':
			print '<command> need more detail help text'
			continue
		elif cmds[0] == 'status':
			print "status"
		elif cmds[0] == 'initdb':
			print "initdb"
			s.send(initdb())
			result = gather_reply(s)
			print result						
		elif cmds[0] == 'resetdb':
			print "resetdb"
			s.send(resetdb())
			result = gather_reply(s)
			print result			
		elif cmds[0] == 'addbr':
			print "addbr"
			if len (cmds) < 2:
				print "missing"
				continue			
			s.send(addbr(cmds[1]))
			result = gather_reply(s)
			print result
		elif cmds[0] == 'delbr':
			print "delbr"
			if len (cmds) < 2:
				print "missing"
				continue			
			s.send(findbr(cmds[1]))
			result = gather_reply(s)
			print result			
			uuid = result["result"][0]["rows"][0]["_uuid"][1]
			print uuid
			s.send(delbr(cmds[1],uuid))
			result = gather_reply(s)
			print result
		elif cmds[0] == 'addport':
			print "addport"
			if len (cmds) < 3:
				print "missing"
				continue			
			s.send(addport(cmds[1], cmds[2]))
			result = gather_reply(s)
			print result
		elif cmds[0] == 'delport':
			print "delport"
			if len (cmds) < 3:
				print "missing"
				continue			
			s.send(findport(cmds[2]))
			result = gather_reply(s)
			print result			
			uuid = result["result"][0]["rows"][0]["_uuid"][1]
			print uuid
			s.send(delport(cmds[1],cmds[2],uuid))
			result = gather_reply(s)
			print result			
		elif cmds[0] == 'insert':
			if len (cmds) < 3:
				print "missing"
				continue
			else:
				sys.stdout.write('insert =')
				print insert(db_name,cmds[1],cmds[2])
				sys.stdout.write('transact =')
				update = transact(insert(db_name,cmds[1],cmds[2]))
				s.send(update)
				uuid = gather_reply(s)
				print uuid
		elif cmds[0] == 'list':
			if len (cmds) < 2:
				print "missing"
				continue
			elif cmds[1] == 'tables':
				print "\ntables\n"
				for table in tables:
					print "---"
					print table
					print "---"
					columns = db_schema['result']['tables'][table]['columns']
					#print columns
					#print columns.values()
					for column in columns:
						sys.stdout.write('|')
						sys.stdout.write(column)
						sys.stdout.write('|')
					#	types = db_schema['result']['tables'][table]['columns'][column]['type']
					#	print types
					print ""
			elif cmds[1] == 'bridges':
				print "\nbridges\n"
				print bridges.values()
				for bridge in bridges.values():
					print "---"
					print bridge['new']['name']
			elif cmds[1] == 'interfaces':
				print "\ninterfaces\n"
				print interfaces.values()
				for interface in interfaces.values():
					print "---"
					print interface['new']['name']
			else:
				print "error"
				continue
		elif cmds[0] == 'add':
			s.send(add_bridge())
			result = gather_reply(s)
			print result			
		elif cmds[0] == 'exit':
			s.close()
			sys.exit()
		else:
			print "invalid command" 	

	#db_schema = get_schema(s, db_name)
	#print db_schema
	#columns = {"Bridge":{"columns":["name"]}}
	#print monitor(s, columns, 1)
	# TODO: Put this in a thread and use Queues to send/recv data from the thread
		
			
	
if __name__ == '__main__':
	main()	
