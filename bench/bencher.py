import subprocess
import datetime
import os
import json 
import sys 
import getopt
# import matplotlib.pyplot as plt 

def analyze_singleton_experiment(input_file):
	with open(input_file) as json_file:
		data = json.load(json_file)
		print(data["slab_hash"]['device_name'])
		trials = data["slab_hash"]["trial"]

		for trial in trials:
			if( abs(trial["query_ratio"]) < 0.000001):
				print("here 1")
				data_q0 = (trial["load_factor"], trial["build_rate_mps"], trial["search_rate_mps"], trial["search_rate_bulk_mps"])
			elif abs(trial["query_ratio"] - 1.0) < 0.000001:
				print("here 2")
				data_q1 = (trial["load_factor"], trial["build_rate_mps"], trial["search_rate_mps"], trial["search_rate_bulk_mps"])

		print("Experiments when none of the queries exist:")
		print("load factor\tbuild rate(M/s)\t\tsearch rate(M/s)\tsearch rate bulk(M/s)")
		print("%.2f\t\t%.3f\t\t%.3f\t\t%.3f" % (data_q0[0], data_q0[1], data_q0[2], data_q0[3]))

		print("Experiments when all of the queries exist:")
		print("load factor\tbuild rate(M/s)\t\tsearch rate(M/s)\tsearch rate bulk(M/s)")
		print("%.2f\t\t%.3f\t\t%.3f\t\t%.3f" % (data_q1[0], data_q1[1], data_q1[2], data_q1[3]))

def analyze_load_factor_experiment(input_file):
	with open(input_file) as json_file:
		data = json.load(json_file)
		print(data["slab_hash"]['device_name'])
		trials = data["slab_hash"]["trial"]

		tabular_data_q0 = []
		tabular_data_q1 = []

		for trial in trials:
			if( abs(trial["query_ratio"]) < 0.000001):
				tabular_data_q0.append((trial["load_factor"], trial["build_rate_mps"], trial["search_rate_mps"], trial["search_rate_bulk_mps"]))
			elif abs(trial["query_ratio"] - 1.0) < 0.000001:
				tabular_data_q1.append((trial["load_factor"], trial["build_rate_mps"], trial["search_rate_mps"], trial["search_rate_bulk_mps"]))

		tabular_data_q0.sort()
		print("Experiments when none of the queries exist:")
		print("load factor\tbuild rate(M/s)\t\tsearch rate(M/s)\tsearch rate bulk(M/s)")
		for pair in tabular_data_q0:
			print("%.2f\t\t%.3f\t\t%.3f\t\t%.3f" % (pair[0], pair[1], pair[2], pair[3]))		

		tabular_data_q1.sort()
		print("Experiments when all of the queries exist:")
		print("load factor\tbuild rate(M/s)\t\tsearch rate(M/s)\tsearch rate bulk(M/s)")
		for pair in tabular_data_q1:
			print("%.2f\t\t%.3f\t\t%.3f\t\t%.3f" % (pair[0], pair[1], pair[2], pair[3]))

def analyze_table_size_experiment(input_file):
	with open(input_file) as json_file:
		data = json.load(json_file)
		print(data["slab_hash"]['device_name'])
		trials = data["slab_hash"]["trial"]

		tabular_data = []

		for trial in trials:
			tabular_data.append((trial["num_keys"], 
				trial['load_factor'], 
				trial['query_ratio'], 
				trial["build_rate_mps"], 
				trial["search_rate_mps"], 
				trial["search_rate_bulk_mps"]))

		tabular_data.sort()
		# print("Experiments when %.2f\% of the queries exist:" % (tabular_data[0][2]))
		print("num keys\tbuild rate(M/s)\t\tsearch rate(M/s)\tsearch rate bulk(M/s)")
		for pair in tabular_data:
			print("%d\t\t%.3f\t\t%.3f\t\t%.3f" % (pair[0], pair[3], pair[4], pair[5]))

def analyze_concurrent_experiment(input_file):
	with open(input_file) as json_file:
		data = json.load(json_file)
		print("GPU hardware: %s" % (data["slab_hash"]['device_name']))
		trials = data["slab_hash"]["trial"]

		tabular_data = []

		for trial in trials:
			tabular_data.append((trial["init_load_factor"], 
				trial['final_load_factor'], 
				trial['num_buckets'], 
				trial["initial_rate_mps"], 
				trial["concurrent_rate_mps"]))

		tabular_data.sort()
		print("===============================================================================================")
		print("Concurrent experiment:")
		print("\tvariable load factor, fixed number of elements")
		print("\tOperation ratio: (insert, delete, search) = (%.2f, %.2f, [%.2f, %.2f])" % (trials[0]['insert_ratio'], trials[0]['delete_ratio'], trials[0]['search_exist_ratio'], trials[0]['search_non_exist_ratio']))
		print("===============================================================================================")
		print("batch_size = %d, init num batches = %d, final num batches = %d" % (trials[0]['batch_size'], trials[0]['num_init_batches'], trials[0]['num_batches']))
		print("===============================================================================================")
		print("init lf\t\tfinal lf\tnum buckets\tinit build rate(M/s)\tconcurrent rate(Mop/s)")
		print("===============================================================================================")
		for pair in tabular_data:
			print("%.2f\t\t%.2f\t\t%d\t\t%.3f\t\t%.3f" % (pair[0], pair[1], pair[2], pair[3], pair[4]))					

def main(argv):
	input_file = ''
	try:
		opts, args = getopt.getopt(argv, "hi:m:", ["help", "ifile=", "mode="])
	except getopt.GetOptError:
		print("bencher.py -i <inputfile>")
		sys.exit(2)
	
	for opt, arg in opts:
		if opt == '-h':
			print("bencher.py -i <inputfile>")
			sys.exit()
		else:
			if opt in ("-i", "--ifile"):
				input_file = arg
			if opt in ("-m", "--mode"):
				mode = int(arg)

		print(" === " + input_file)
	# if the input file is not given, proper experiments should be run first
	if not input_file:		
		# == creating a folder to store results
		out_directory = "../build/bench_result/"
		if (not os.path.isdir(out_directory)):
			os.mkdir(out_directory)

		# == running benchmark files
		bin_file = "../build/bin/benchmark"
		if(not os.path.exists(bin_file)):
			raise Exception("binary file " + bin_file + " not found!")

		# creating a unique name for the file
		cur_time_list = str(datetime.datetime.now()).split()
		out_file_name = "out"
		for s in cur_time_list:
			out_file_name += ("_" + s)

		out_file_dest = out_directory + out_file_name + ".json"
		input_file = out_file_dest # input file for the next step
		print(" == filename = " + out_file_dest)

		print("mode = %d" % mode)
		if mode == 0:
			args = (bin_file, "-mode", str(mode), 
				"-num_key", str(2**22),
				"-expected_chain", str(0.6),
				"-filename", out_file_dest)
		elif mode == 1:
			args = (bin_file, "-mode", str(mode), "-filename", out_file_dest)
		elif mode == 2:
				args = (bin_file, "-mode", str(mode), 
				"-nStart", str(18),
				"-nEnd", str(23), 
				"-expected_chain", str(0.6),
				"-query_ratio", str(1.0),
				"-filename", out_file_dest)
		elif mode == 3:
			args = (bin_file, "-mode", str(mode),
			"-nStart", str(18),
			"-nEnd", str(21),
			"-num_batch", str(4),
			"-init_batch", str(3),
			"-lf_conc_step", str(0.1),
			"-lf_conc_num_sample", str(10),  
			"-filename", out_file_dest)

		print(" === Started benchmarking ... ")

		popen = subprocess.Popen(args, stdout = subprocess.PIPE)
		popen.wait()

		output = popen.stdout.read()
		print(output)
		print(" === Done!")
	elif not os.path.exists(input_file):
		raise Exception("Input file " + input_file + " does not exist!")

	# reading the json files:
	if mode == 0:
		analyze_singleton_experiment(input_file)
	elif mode == 1:
		analyze_load_factor_experiment(input_file)
	elif mode == 2:
		analyze_table_size_experiment(input_file)
	elif mode == 3:
		analyze_concurrent_experiment(input_file)	
	else:
		print("Invalid mode entered")
		sys.exit(2)

if __name__ == "__main__":
	main(sys.argv[1:])					