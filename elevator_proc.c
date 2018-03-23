#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/linkage.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/delay.h> //sleep
#include <linux/list.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/mutex.h>
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple Elevator Kernel");


#define ENTRY_NAME "elevator"
#define ENTRY_SIZE 1024
#define PERMS 0644
#define PARENT NULL

// passenger types
#define ADULT 1
#define CHILD 2
#define ROOM_SERVICE 3
#define BELLHOP 4

// elevator capacity
#define MAX_WEIGHT 30
#define MAX_UNITS 10

// time constants
#define MOVE_TIME 2
#define LOAD_TIME 1

#define START_FLOOR 1

// elevator states
#define OFFLINE 0
#define IDLE 1
#define LOADING 2
// below are saved in twice in struct
// when we switch state to loading we want to be able 
// to resume in the same movement directions if there are more
// passengers going that directions already on a queue
#define DOWN 3
#define UP 4



static struct file_operations fops;
static char *message;
static int read_p;
struct task_struct *elevator_thread;
struct mutex floors_l_mutex;
struct mutex elevator_l_mutex;
/* 
			System Calls functions listed below
			they are external -> defined in sys_call.c
			int start_elevator(void) :
				Activates the elevator for service. 
				From that point onward, the elevator exists and will begin to service requests. 
				return 1 if the elevator is already active
				0 for a successful elevator start
				-ERRORNUM if it could not initialize (e.g. -ENOMEM if it couldn’t allocate memory).
			
			int issue_request(int passenger_type, int start_floor, int destination_floor):
				Creates a passenger of type @passenger_type at @start_floorthat wishes to go to @destination_floor
				This function returns 1 if the request is not valid (one of the variables is out of range)
				0 otherwise 

			int stop_elevator: 
				Turn the elevator off, 
				Deliver people in the elevator, do not load more passengers.
				set elevator status to OFFLINE 


*/
/*
			elevator type represents the elevator object 
			status: idle, offline, loading, up, down 
			int w_load: weight
			int unit_load: number of passengers 
			int floor: floor number
			int direction: UP/DOWN (defined as 4/3)
			int next_stop: next floor intended to service
			int shutdown: 1 or 0, if 1 start shutdown procedure, don't accept more passengers

*/

struct {
	int status; 
	int w_load; 
	int unit_load; 
	int floor;
	int direction; 
	int next_stop; 
	int shutdown; 
	int serviced;
	int up_bound;
	int low_bound;
	int served_per_fl[10];

	struct list_head p_list;
} elevator;



/*
			passenger type [ADULT or CHILD or ROOM_SERVICE or BELLHOP]
			used to store passenger information after an elevator request
			weight: 1, 2, 4, 6
			units: 1,2
			start: initial floor (1-10)
			destination: drop off (make sure different then start and between 1-10)

*/
typedef struct passenger {
	int weight;
	int units; 
	int start;
	int destination;
	int type;

	struct list_head list;
} Passenger;



/* 
			Defining an array of the waiting lists for floors. 
*/

struct list_head floors[10];

/*
			init waiting lists in the array of floors.
*/
int init_floor_lists(void) {

	int i;
	for (i = 0; i < 10; ++ i){
		INIT_LIST_HEAD(&floors[i]);
	}

	return 0;
}



/* 
			Definining functions required for system calls.
*/

long start_elevator(void);
long issue_request(int, int, int);
long stop_elevator(void);

/*
			Function that initializes a global object elevator.
			Triggered by a system call.
*/
extern long (*STUB_start_elevator)(void);
long start_elevator(void){
	int i;
	if (elevator.status == IDLE || elevator.status == LOADING || elevator.status == UP || elevator.status == DOWN)
		return 1;
	else {
		printk("Starting elevator\n");

		elevator.status = IDLE;
		elevator.w_load = 0;
		elevator.unit_load = 0;
		elevator.floor = 1; 
		elevator.up_bound = -1;
		elevator.low_bound = -1;
		elevator.next_stop = -2;
		elevator.shutdown = -1;
		elevator.serviced = 0;
		
		for (i = 0; i < 10; ++i){
			elevator.served_per_fl[i] = 0;
		}
		// init the list of passengers in the elevator 
		INIT_LIST_HEAD(&elevator.p_list);
		// create an array of linked lists 
		init_floor_lists();
		return 0;
	}
	
}

/*
			Function that places a passenger on a waiting list.
			Triggered by a system call.
*/
extern long (*STUB_issue_request)(int, int, int);
long issue_request(int passenger_type, int start_floor, int destination_floor){
	if ( (passenger_type > 4) || (passenger_type < 1) )
		return 1;
	else if ( (start_floor < 1) || (start_floor > 10) )
		return 1;
	else if ( (destination_floor < 1) || (destination_floor > 10) )
		return 1;
	else {
		Passenger *p;
		int weight;
		int units;
		switch (passenger_type) {
			case ADULT:
				weight = 2;
				units = 1;
				break;
			case CHILD:
				weight = 1;
				units = 1;
				break;
			case ROOM_SERVICE:
				weight = 4;
				units = 2;
				break;
			case BELLHOP:
				weight= 6;
				units = 2;
				break;
			default:
				return 1;
		}
		p = kmalloc(sizeof(Passenger) * 1, __GFP_RECLAIM);
		if (p == NULL)
			return -ENOMEM;

		p->weight = weight;
		p->units = units;
		p->start = start_floor;
		p->destination = destination_floor;
		p->type = passenger_type; 

		mutex_lock_interruptible(&floors_l_mutex);
		list_add_tail(&p->list, &floors[start_floor - 1]); /* insert at back of list */
		mutex_unlock(&floors_l_mutex);
	}

	return 0;
}

/*
			Function that turns off the elevator.
			Triggered by a system call
*/
extern long (*STUB_stop_elevator)(void);
long stop_elevator(void){

	mutex_lock_interruptible(&elevator_l_mutex);
	
	if (elevator.status == OFFLINE || elevator.shutdown == 1){
		mutex_unlock(&elevator_l_mutex);
		return 1;
	}
	elevator.shutdown = 1;
	mutex_unlock(&elevator_l_mutex);
	return 0;
	
}

/* 
			Place people from a floor floor_no in the elevator,if there is enough room.
			If elevator was empty, update the direction.
			Update bounds, next_stop, weight and unit load. 
*/
void load_elevator(int floor_no) {
	struct list_head *temp;
	struct list_head *dummy;
	Passenger *a;

	//printk("Made it to the load function\n");

	/* move items to a temporary list to illustrate movement */
	//list_for_each_prev_safe(temp, dummy, &animals.list) { /* backwards */
	list_for_each_safe(temp, dummy, &floors[floor_no]) { /* forwards */
		a = list_entry(temp, Passenger, list);
		printk("The MAX_WEIGHT is: %d, the w_load is: %d, and the passenger weight is %d\n", MAX_WEIGHT, elevator.w_load, a->weight);
		printk("The MAX_UNITS is: %d, the unit_load is: %d, and the passenger unit is %d\n", MAX_UNITS, elevator.unit_load, a->units);
		if ( (elevator.w_load + a->weight <= MAX_WEIGHT) && (elevator.unit_load + a->units <= MAX_UNITS) ) {
			printk("Case 1");
			// elevator changes direction only when empty
			// first person that enters sets the direction
			if (list_empty(&elevator.p_list)){ 
				printk("Case 2");
				if (a -> destination > elevator.floor){
					printk("Case 3");
					elevator.direction = UP;
					elevator.up_bound = a -> destination;
					elevator.next_stop = a -> destination;
				}	
				else{
					printk("Case 4");
					elevator.direction = DOWN;
					elevator.low_bound = a -> destination;
					elevator.next_stop = a -> destination;
				}
				//Add case where a passenger wants to stay on the same floor
				printk("Case 5");
				list_move_tail(temp, &elevator.p_list); /* move to back of list */
				elevator.w_load += a->weight;
				elevator.unit_load += a->units;
				// add floor serviced here 
			}
			else if (elevator.direction == UP && a->destination > elevator.floor) {
				printk("Case 6");
				list_move_tail(temp, &elevator.p_list); /* move to back of list */
				if (a->destination > elevator.up_bound){
					elevator.up_bound = a -> destination;
					printk("Case 7");
				}
				else if (a -> destination < elevator.next_stop){
					elevator.next_stop = a -> destination;
					printk("Case 8");
				}
				elevator.w_load += a->weight;
				elevator.unit_load += a->units;
			}
			else if (elevator.direction == DOWN && a->destination < elevator.floor) {
				printk("Case 9");
				list_move_tail(temp, &elevator.p_list); /* move to back of list */
				if (a->destination < elevator.low_bound){
					elevator.low_bound = a -> destination;
					printk("Case 10");
				}
				else if (a -> destination > elevator.next_stop){
					elevator.next_stop = a -> destination;
					printk("Case 11");
				}
				printk("Case 12");
				elevator.w_load += a->weight;
				elevator.unit_load += a->units;
			}	
		}
	}
}

/* 
			Unload people from the elevator if floor_no equals their destination 
*/
void unload_elevator(int floor_no){
	struct list_head move_list;
	struct list_head *temp;
	struct list_head *dummy;
	int i;
	Passenger *a;

	INIT_LIST_HEAD(&move_list);

	/* move items to a temporary list to illustrate movement */
	//list_for_each_prev_safe(temp, dummy, &animals.list) { /* backwards */
	list_for_each_safe(temp, dummy, &elevator.p_list) { /* forwards */
		a = list_entry(temp, Passenger, list);

		if (a->destination == floor_no) {
			list_move_tail(temp, &move_list); /* move to back of list */
			elevator.w_load -= a->weight;
			elevator.unit_load -= a->units;
			elevator.serviced += 1;
			elevator.served_per_fl[(a->start)-1] += 1;
		}
	}	

	/* print stats of list to syslog, entry version just as example (not needed here) */
	i = 0;
	list_for_each_entry(a, &move_list, list) { /* forwards */
		/* can access a directly e.g. a->id */
		i++;
	}
	printk(KERN_NOTICE "%d people left the elevator", i);

	/* free up memory allocation of Animals */
	//list_for_each_prev_safe(temp, dummy, &move_list) { /* backwards */
	list_for_each_safe(temp, dummy, &move_list) { /* forwards */
		a = list_entry(temp, Passenger, list);
		list_del(temp);	/* removes entry from list */
		kfree(a);
	}
}

/*
			CHeck if any passenger reached his/her destination
*/
int should_unload(int f){
	struct list_head *temp;
	Passenger *p;
	// read only access, no need for for_each_safe
	list_for_each(temp, &elevator.p_list){
		p = list_entry(temp, Passenger, list);
		
		if (f == p->destination){
			return 1; // return true if needs to unload
		}
	}

	return 0; // return false if no one is getting out
}

/* 
			Find the closest non epmty floor or set elevator to idle and return -1 if all floors are empty 
*/
int empty_find_next_stop(void){
	int closest = 100; // to make sure it get updated on the first check
	int i;

	if (elevator.shutdown == 1)
		return -1;

	for (i = 1; i < 11; ++i){
		if (abs(elevator.floor - i) < abs(elevator.floor - closest) && !list_empty(&floors[i-1])){
			closest = i;
		}
	}
	// check if there was anyone waiting, if not set to idle
	if ( closest != 100 ){ 
		elevator.next_stop = closest;
		if (elevator.next_stop > elevator.floor){
			elevator.up_bound = closest; // the highest level with a passeneger on it
			elevator.direction = UP;
			elevator.status = UP;
		}	
		else{
			elevator.low_bound = closest; // the lowest level with a passeneger on it
			elevator.direction = DOWN;
			elevator.status = DOWN;
		}
		return closest;
	}

	elevator.status = IDLE;
	elevator.next_stop = -1;
	elevator.low_bound = -1;
	elevator.up_bound = -1;

	return -1;
	
}

/* 			
			check, and let them do it, if anyone wants to get out on the floor above/below
			check, and let them do it, if anyone wants to get in on the floor above/below
			set floor to floor +/- 1
			return direction or -1 if empty
*/
void move_one(int direction){

	if (direction == UP)
		elevator.floor += 1;
	else
		elevator.floor -= 1;

	if (should_unload(elevator.floor)){ 
		elevator.status = LOADING;
		//ssleep(LOAD_TIME);
		unload_elevator(elevator.floor);
		if(!list_empty(&floors[elevator.floor - 1]) && elevator.shutdown != 1){
			load_elevator(elevator.floor - 1);
		}
	}
	else if (!list_empty(&floors[elevator.floor]) && elevator.shutdown != 1){
		elevator.status = LOADING;
		//ssleep(LOAD_TIME);
		load_elevator(elevator.floor -1);
	}

	// Restore Status after loading 
	// If no loading happened should not cause change
	// If elevator empty and no requests, status will be changed after this function returns 
	elevator.status = elevator.direction;
}

/*  
			Main algorithm, modified SCAN, Works like a "classic" elevator
			If for instance direction is UP it goes on the highest floor requested,
			and drops off/picks up passengers, going the same direction, on its way.
			Changes direction only when empty.
			Many optimizations possible
*/

int run_elevator(void* params){
	
	while (!kthread_should_stop())
	{
		mutex_lock_interruptible(&elevator_l_mutex);
		if (elevator.status != OFFLINE)
		{			

			mutex_lock_interruptible(&floors_l_mutex);	
			if ( list_empty(&elevator.p_list) ){
				
				int ret = empty_find_next_stop();
				if (elevator.shutdown == 1){
					elevator.status = OFFLINE;
				}
				else if (ret == elevator.floor && elevator.shutdown != 1){
					load_elevator(elevator.floor - 1);
				}
				else if (ret != -1){
					move_one(elevator.direction);
				}
				
			}			
			else
			{

				move_one(elevator.direction);
				
				if (list_empty(&elevator.p_list))
					empty_find_next_stop();
					
			}
			mutex_unlock(&floors_l_mutex);
		}
		mutex_unlock(&elevator_l_mutex);
	}
	return 0;	
} 

/*
			Get the total weight of wait list on @floor_no
*/
int floor_w_load(int floor_no){
	struct list_head *temp;
	Passenger *p;
	int weight = 0;
	// read only access, no need for for_each_safe
	list_for_each(temp, &floors[floor_no]){
		p = list_entry(temp, Passenger, list);
		weight += p-> weight;
	}
	return weight;
}

/*
			Get the total units of wait list on @floor_no
*/
int floor_u_load(int floor_no){
	struct list_head *temp;
	Passenger *p;
	int units = 0;
	// read only access, no need for for_each_safe
	list_for_each(temp, &floors[floor_no]){
		p = list_entry(temp, Passenger, list);
		units += p-> units;
	}
	return units;
}

/*
			create a report from elevator and the building
*/
void print_stats(char* message){
	int i;
	char buffer[512];
	sprintf(message, "\nElevator Report:\n");
	sprintf(buffer, "Elevator Status: %d\n Elevator Floor: %d\n  Elevator Next Floor: %d\n Weight Load: %d\n Unit Load: %d\n", elevator.status, elevator.floor, elevator.next_stop, elevator.w_load, elevator.unit_load);
	strcat(message, buffer);
	sprintf(buffer, "\nBuilding Report:\n");
	strcat(message, buffer);
	for (i = 0; i < 10; ++i){
		sprintf(buffer, "Floor %d:\n People Serviced: %d\n Current Weight Load: %d\n Current Unit Load: %d\n", i+1, elevator.served_per_fl[i], floor_w_load(i), floor_u_load(i));
		strcat(message, buffer);
	}
}

int elevator_proc_open(struct inode *sp_inode, struct file *sp_file) {
	printk(KERN_INFO "proc called open\n");
	
	read_p = 1;
	message = kmalloc(sizeof(char) * ENTRY_SIZE, __GFP_RECLAIM | __GFP_IO | __GFP_FS);
	if (message == NULL) {
		printk(KERN_WARNING "elevator_proc_open");
		return -ENOMEM;
	}

	mutex_lock_interruptible(&elevator_l_mutex);
	mutex_lock_interruptible(&floors_l_mutex);
	print_stats(message);
	mutex_unlock(&floors_l_mutex);
	mutex_unlock(&elevator_l_mutex);
	
	return 0;
}

ssize_t elevator_proc_read(struct file *sp_file, char __user *buf, size_t size, loff_t *offset) {
	int len = strlen(message);
	
	read_p = !read_p;
	if (read_p)
		return 0;
		
	printk(KERN_INFO "proc called read\n");
	copy_to_user(buf, message, len);
	return len;
}

int elevator_proc_release(struct inode *sp_inode, struct file *sp_file) {
	printk(KERN_NOTICE "proc called release\n");
	kfree(message);
	return 0;
}
/*
		Initialize the module
*/
static int elevator_init(void) {
	printk(KERN_NOTICE "/proc/%s create\n",ENTRY_NAME);
	fops.open = elevator_proc_open;
	fops.read = elevator_proc_read;
	fops.release = elevator_proc_release;
	
	// assign functions to function pointers for sys calls
	STUB_start_elevator = start_elevator;
	STUB_issue_request = issue_request;
	STUB_stop_elevator = stop_elevator;

	if (!proc_create(ENTRY_NAME, PERMS, NULL, &fops)) {
		printk(KERN_WARNING "proc create\n");
		remove_proc_entry(ENTRY_NAME, NULL);
		return -ENOMEM;
	}

	//initialize the locks
	mutex_init(&floors_l_mutex);
	mutex_init(&elevator_l_mutex);

	// create a thread for our elevator
	elevator_thread = kthread_run(run_elevator, NULL, "elevator_thread");
	if (IS_ERR(elevator_thread)) {
		printk(KERN_WARNING "error spawning thread");
		remove_proc_entry(ENTRY_NAME, NULL);
		return PTR_ERR(elevator_thread);
	}
	return 0;
}
module_init(elevator_init);

/*
			Uninstall the module
*/
static void elevator_exit(void) {
	int elevator_ret;

	// set sys call function pointers to NULLs
	STUB_start_elevator = NULL;
	STUB_issue_request = NULL;
	STUB_stop_elevator = NULL;

	elevator_ret = kthread_stop(elevator_thread);
	if (elevator_ret != -EINTR)
		printk("Elevator thread has stopped\n");
	remove_proc_entry(ENTRY_NAME, NULL);
	printk(KERN_NOTICE "Removing /proc/%s\n", ENTRY_NAME);
}
module_exit(elevator_exit);
