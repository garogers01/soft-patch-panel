#!/usr/bin/python

import sys, getopt, time, select
# Import all from module socket
from socket import *
#Importing all from thread
from thread import *
#Importing all from queue inter thread communication
from Queue import Queue

threads = []
thread_count = 0;
prompt = 'main > '
main_queue = Queue()
server = False

def connectionthread(name, id, conn, queue):
	global server
	print 'thread: ', name, id
	sys.stdout.write(prompt)
	cmd = 'hello'
	#infinite loop so that function do not terminate and thread do not end.
	while True:
		try:
			ready_to_read, ready_to_write, in_error = \
				select.select([conn,], [conn,], [], 5)
		except select.error:
			break
		#Sending message to connected client
		try:
			cmd = queue.get(True)
			#print 'client thread: cmd', cmd
			conn.send(cmd) #send only takes string
		except KeyError:
			break
		except Exception, e:
			print str(e) 
			break
		#Receiving from client
		try:
			data = conn.recv(1024) # 1024 stands for bytes of data to be received
			print ''
			print "clientthread data: ", data
			if len(data) == 0:
				print "clientthread data: zero"
				break
		except Exception, e:
			print str(e)
			break
		
	if name == "client":
		print "client thread: connection closed"
		print "client thread: threads length", len(threads)
		threads.remove(queue)
	if name == "server":
		server = False
		print "server thread: connection closed"
	
	conn.close()
	
def acceptthread(sock,):
	global server
	global threads
	global main_queue
	print 'accept thread started'
	sys.stdout.write(prompt)
	while True:
		#Accepting incoming connections
		conn, addr = sock.accept()
		#Creating new thread. Calling clientthread function for this function and passing conn as argument.
		
		if server == False:
			start_new_thread(connectionthread,('server','0',conn,main_queue,)) 
			server = True
		else:	
			queue=Queue()
			start_new_thread(connectionthread,('client', len(threads),conn,queue,)) 
			threads.append(queue)
		#start new thread takes 1st argument as a function name to be run, second is the tuple of arguments to the function.
	sock.close()
		
def main(argv):
	# Defining server address and port
	host = ''  #'localhost' or '127.0.0.1' or '' are all same
	port = 52000
	input = ''
	thread_id = 9999 # Invalid
	cmd = ''
	thread_queue = ''
	
	try:
		opts, args = getopt.getopt(argv,"hp:",["port="])
	except getopt.GetoptError:
		print 'controller.py -p <port_number>'
		sys.exit(2)
	for opt, arg in opts:
		if opt == '-h':
			print 'controller.py -p <port_number>'
			sys.exit()
		elif opt in ("-p", "--port"):
			port = int(arg)
			print 'port is "', port

	#Creating socket object
	sock = socket()
	#Binding socket to a address. bind() takes tuple of host and port.
	sock.bind((host, port))
	#Listening at the address
	sock.listen(16) #5 denotes the number of clients can queue
	
	start_new_thread(acceptthread,(sock,)) 

	while True:
		try:
			input = raw_input(prompt)
		except (KeyError, KeyboardInterrupt) as e:
			print "exit" 
			break
		if input == 'h' or input == '?':
			print '<client id>;<command>'
			continue
		outputs = input.split ('|')	
		for output in outputs:
			cmds = output.split (';', 1)	
			if len (output) < 2:
				continue
			elif cmds[0] == 'pp':
				#print "main: number of elements in threads ", len(threads)
				#print "queue block passed"
				main_queue.put(cmds[1])			
			else:
				thread_id = int (cmds[0])
				#print "main: number of elements in threads ", len(threads)
				thread_queue = threads[thread_id]
				#print "queue block passed"
				thread_queue.put(cmds[1])			
	sock.close()
	
if __name__ == "__main__":
   main(sys.argv[1:])
