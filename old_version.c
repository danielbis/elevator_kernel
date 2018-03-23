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
#include <linux/stdlib.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/mutex.h>
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple Elevator Kernel");


#define ENTRY_NAME "elevator"
#define ENTRY_SIZE 256
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

	int run_elevator: 
		temporary function to tick the elevator
		will be removed when elevator gets own thread
		returns 0 on success


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



// array of the floors 
//int floors[10] = {0,0, ADULT, 0, CHILD, 0, BELLHOP, 0, ADULT, 0};
struct list_head floors[10];

// init lists in the array 
int init_floor_lists(void) {

	int i;
	for (i = 0; i < 10; ++ i){
		INIT_LIST_HEAD(&floors[i]);
	}

	return 0;
}



/* 
	Definining required functions below
*/

long start_elevator(void);
long issue_request(int, int, int);
long stop_elevator(void);

//////////////////////////// defining a global object elevator ///////////////////////////////
extern long (*STUB_start_elevator)(void);
long start_elevator(void){
	if (elevator.status == IDLE || elevator.status == LOADING || elevator.status == UP || elevator.status == DOWN)
		return 1;
	else {
		printk("Starting elevator\n");

		elevator.status = IDLE;
		elevator.w_load = 0;
		elevator.unit_load = 0;
		elevator.floor = 1; 
		elevator.serviced = 0;
		// init the list of passengers in the elevator 
		INIT_LIST_HEAD(&elevator.p_list);
		// create an array of linked lists 
		init_floor_lists();
		return 0;
	}
	
}


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
		list_add_tail(&p->list, &floors[start_floor]); /* insert at back of list */
		mutex_unlock(&floors_l_mutex);
	}

	return 0;
}

// Place people from a floor floor_no in the elevator if there is enough room 
void load_elevator(int floor_no) {
	struct list_head *temp;
	struct list_head *dummy;
	Passenger *a;

	/* move items to a temporary list to illustrate movement */
	//list_for_each_prev_safe(temp, dummy, &animals.list) { /* backwards */
	list_for_each_safe(temp, dummy, &floors[floor_no]) { /* forwards */
		a = list_entry(temp, Passenger, list);

		if ( (elevator.w_load + a->weight <= MAX_WEIGHT) && (elevator.unit_load + a->units <= MAX_UNITS) ) {
			// elevator changes direction only when empty
			// first person that enters sets the direction
			if (list_empty(&elevator.p_list)){ 
				if (a -> destination > elevator.floor){
					elevator.direction = UP;
				}	
				else{
					elevator.direction = DOWN;
\				}
				
				list_move_tail(temp, &elevator.p_list); /* move to back of list */
				elevator.w_load += a->weight;
				elevator.unit_load += a->units;
			}
			else if ( (elevator.direction == UP && a->destination > elevator.floor) || (elevator.direction == DOWN && a->destination < elevator.floor) )
				list_move_tail(temp, &elevator.p_list); /* move to back of list */
				elevator.w_load += a->weight;
				elevator.unit_load += a->units;
		}
	}	
}


// Unload people from the elevator if floor_no equals their destination 
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

int find_next_stop(){
	struct list_head *temp;
	Passenger *p;
	int closest = 100; // to make sure it get updated on the first check

	// read only access, no need for for_each_safe
	list_for_each(temp, &elevator.p_list){
		p = list_entry(temp, Passenger, list);
		
		if (abs(elevator.floor - (p -> destination) ) < abs(elevator.floor - closest)){

			// if we just want to go to the closest one delete if statements
			if (elevator.direction == UP && p -> destination > elevator.floor){
				closest = p -> destination;
			}
			else if(elevator.direction == DOWN && p -> destination < elevator.floor){
				closest = p -> destination;
			}
		}
	}

	if (closest > elevator.floor){
		elevator.direction = UP;
		elevator.status = UP;
	}	
	else{
		elevator.direction = DOWN;
		elevator.status = DOWN;
	}
	return closest;
}


//extern long (*STUB_run_elevator)(void);
long run_elevator(void){
	
	while (!kthread_should_stop()){
		
		Passenger *p;
		
		mutex_lock_interruptible(&elevator_l_mutex);
		mutex_lock_interruptible(&floors_l_mutex);
		
		if ( list_empty(&elevator.p_list) ){
			int closest = 100; // to make sure it get updated on the first check
			int i;
			for (i = 0; i < 10; ++i){
				if (abs(elevator.floor - (i+1)) < abs(elevator.floor - closest) && !list_empty(&floors[i])){
					closest = i + 1;
				}
			}
			
			// check if there was anyone waiting, if not set to idle
			if ( closest != 100 ){ 
				elevator.next_stop = closest;

				if (elevator.next_stop > elevator.floor){
					elevator.direction = UP;
					elevator.status = UP;
				}	
				else{
					elevator.direction = DOWN;
					elevator.status = DOWN;
				}

				int floor_diff = abs(elevator.floor - elevator.next_stop);
				// sleep i *MOVE_TIME once you stop to simulate ride time
				// check the floors between current and destination
				mutex_unlock(&floors_l_mutex);
				mutex_unlock(&elevator_l_mutex);
				
				for (i = 1; i < floor_diff - 1; ++i){
					ssleep(MOVE_TIME);
					mutex_lock_interruptible(&elevator_l_mutex);
					mutex_lock_interruptible(&floors_l_mutex);
					if (elevator.direction == UP){
						if (!list_empty(&floors[elevator.floor+i - 1]) && elevator.w_load < MAX_WEIGHT && elevator.unit_load < MAX_UNITS)
						{
							elevator.floor = elevator.floor +  i;
							elevator.status = LOADING;
							
							mutex_unlock(&floors_l_mutex);
							mutex_unlock(&elevator_l_mutex);
							
							ssleep(LOAD_TIME); // loading, wait
							
							mutex_lock_interruptible(&elevator_l_mutex);
							mutex_lock_interruptible(&floors_l_mutex);
							
							load_elevator(elevator.floor-1); // make sure you are loading ppl going in the same direction
							elevator.status = elevator.direction; // restore old status after loading
							
							mutex_unlock(&floors_l_mutex);
							mutex_unlock(&elevator_l_mutex);

							break;
						}
					}
					if (elevator.direction == DOWN){
						if (!list_empty(&floors[elevator.floor-i -1]) && elevator.w_load < MAX_WEIGHT && elevator.unit_load < MAX_UNITS)
						{
							elevator.floor = elevator.floor -  i;
							elevator.status = LOADING;
							mutex_unlock(&floors_l_mutex);
							mutex_unlock(&elevator_l_mutex);
							
							ssleep(LOAD_TIME); // loading, wait

							mutex_lock_interruptible(&elevator_l_mutex);
							mutex_lock_interruptible(&floors_l_mutex);

							load_elevator(elevator.floor-1); // make sure you are loading ppl going in the same direction
							elevator.status = elevator.direction; // restore old status after loading
							
							mutex_unlock(&floors_l_mutex);
							mutex_unlock(&elevator_l_mutex);

							break;
						}
					}
					
					mutex_unlock(&floors_l_mutex);
					mutex_unlock(&elevator_l_mutex);
				}

				//	if we got to the destination without loading (elevator still empty), then load passengers here 
				//	However, if we loaded passengers on the way here, dont do it, break out and set the floor 
				// 	to the last floor we were loading from.
				mutex_lock_interruptible(&elevator_l_mutex);
				mutex_lock_interruptible(&floors_l_mutex);

				floor_diff = abs(elevator.floor - elevator.next_stop); // update
				if (list_empty(&elevator.p_list)){

					mutex_unlock(&floors_l_mutex);
					mutex_unlock(&elevator_l_mutex);
					
					ssleep(floor_diff*MOVE_TIME);

					mutex_lock_interruptible(&elevator_l_mutex);
					mutex_lock_interruptible(&floors_l_mutex);

					elevator.floor = elevator.next_stop;
					elevator.status = LOADING;
					
					mutex_unlock(&floors_l_mutex);
					mutex_unlock(&elevator_l_mutex);

					ssleep(LOAD_TIME);

					mutex_lock_interruptible(&elevator_l_mutex);
					mutex_lock_interruptible(&floors_l_mutex);
					
					load_elevator(elevator.floor - 1);
					elevator.status = elevator.direction;
					mutex_unlock(&floors_l_mutex);
					mutex_unlock(&elevator_l_mutex);
				}
			} else{
				mutex_lock_interruptible(&elevator_l_mutex);

				if (elevator.floor != 5)
					ssleep(MOVE_TIME * (elevator.floor - 5));
				elevator.floor = 5;
				elevator.next_stop = 5;
				elevator.status = IDLE;
				mutex_unlock(&elevator_l_mutex);
			}
		}	
		else{
			struct list_head *temp;
			struct list_head *dummy;
			Passenger *a;

			// if elevator.floor == elevator.next_stop 
			// find new next stop
			mutex_lock_interruptible(&elevator_l_mutex);

			if (elevator.floor == elevator.next_stop){
				elevator.next_stop = find_next_stop();
			}

			mutex_unlock(&elevator_l_mutex);

			mutex_lock_interruptible(&elevator_l_mutex);
			int floor_diff = abs(elevator.floor - elevator.next_stop);
			
			if (elevator.status == UP){
				int i;
				for(i = 0; i <= floor_diff; ++i){
					
					mutex_unlock(&elevator_l_mutex);
					
					ssleep(MOVE_TIME);
					
					mutex_lock_interruptible(&elevator_l_mutex);
					
					list_for_each_safe(temp, dummy, &elevator.p_list) { /* forwards */
						a = list_entry(temp, Passenger, list);

						if (a->destination == elevator.floor + i) {
							elevator.status = LOADING;
							mutex_unlock(&elevator_l_mutex);

							ssleep(LOAD_TIME);
							mutex_lock_interruptible(&elevator_l_mutex);
							unload_elevator(elevator.floor + i -1);
						}
					}
					mutex_lock_interruptible(&floors_l_mutex);
					if (!list_empty(&floors[elevator.floor + i - 1])){
						// if someone has gotten out on that floor, we have loading status 
						// and we already slept, but if not, we have to set it and sleep
						if (elevator.status != LOADING){
							elevator.status = LOADING;
							mutex_unlock(&floors_l_mutex);
							mutex_unlock(&elevator_l_mutex);
							
							ssleep(LOAD_TIME);
							
							mutex_lock_interruptible(&elevator_l_mutex);
							mutex_lock_interruptible(&floors_l_mutex);
						}
						
						
						
						load_elevator(elevator.floor + i - 1);
					}
					mutex_unlock(&floors_l_mutex);
					elevator.status = elevator.direction; // finished unloading/loading, set status back to moving			
				}
			}
			mutex_unlock(&elevator_l_mutex);

			mutex_lock_interruptible(&elevator_l_mutex);
			if (elevator.status == DOWN){
				int i;
				for(i = 0; i <= floor_diff; ++i){
					mutex_unlock(elevator_l_mutex);

					ssleep(MOVE_TIME);

					mutex_lock_interruptible(&elevator_l_mutex);

					list_for_each_safe(temp, dummy, &elevator.p_list) { /* forwards */
						a = list_entry(temp, Passenger, list);

						if (a->destination == elevator.floor - i) {
							elevator.status = LOADING;
							mutex_unlock(&elevator_l_mutex);

							ssleep(LOAD_TIME);
							mutex_lock_interruptible(&elevator_l_mutex);

							unload_elevator(elevator.floor - i - 1);
						}
					}
					mutex_lock_interruptible(&floors_l_mutex);
					if (!list_empty(&floors[elevator.floor - i - 1])){
						// if someone has gotten out on that floor, we have loading status 
						// and we already slept, but if not, we have to set it and sleep
						if (elevator.status != LOADING){
							elevator.status = LOADING;
							
							mutex_unlock(&floors_l_mutex);
							mutex_unlock(&elevator_l_mutex);
							
							ssleep(LOAD_TIME);
							
							mutex_lock_interruptible(&elevator_l_mutex);
							mutex_lock_interruptible(&floors_l_mutex);
						}
						load_elevator(elevator.floor - i - 1);
					}
					mutex_unlock(&floors_l_mutex);
					elevator.status = elevator.direction; // finished unloading/loading, set status back to moving					
				}
			}
			mutex_unlock(&elevator_l_mutex);

			mutex_lock_interruptible(&elevator_l_mutex);
			if (list_empty(&elevator.p_list)){ // if we emptied the list out, change settings
				if (elevator.floor != 5)
					ssleep(MOVE_TIME * (elevator.floor - 5));
				elevator.floor = 5;
				elevator.next_stop = 5;
				elevator.status = IDLE;
			}
			mutex_unlock(&elevator_l_mutex);
		}//end else
	} 
	return 0;	
} 



void print_stats(char* message){

	mutex_lock_interruptible(&elevator_l_mutex);
	sprintf(message, "\nElevator Report:\n");
	sprintf(message, "Elevator Status: %d\n Elevator Floor: %d\n Weight Load: %d\n Unit Load: %d\n", elevator.status, elevator.floor, elevator.w_load, elevator.unit_load);
	mutex_unlock(&elevator_l_mutex);
	mutex_lock_interruptible(&floors_l_mutex);
	sprintf(message, "\nBuilding Report:\n");
	sprintf(message, "Floor Load"); //calculate the load 
	mutex_unlock(&floors_l_mutex);
}

int elevator_proc_open(struct inode *sp_inode, struct file *sp_file) {
	printk(KERN_INFO "proc called open\n");
	
	read_p = 1;
	message = kmalloc(sizeof(char) * ENTRY_SIZE, __GFP_RECLAIM | __GFP_IO | __GFP_FS);
	if (message == NULL) {
		printk(KERN_WARNING "elevator_proc_open");
		return -ENOMEM;
	}

	print_stats(message);
	
	/*if (elevator.status == IDLE || elevator.status == LOADING || elevator.status == UP || elevator.status == DOWN)
		sprintf(message, "Number of people serviced: %d\n Elevator status: %d\n", elevator.serviced, elevator.status);
	else
		sprintf(message, "OFFLINE");*/

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

static int elevator_init(void) {
	printk(KERN_NOTICE "/proc/%s create\n",ENTRY_NAME);
	fops.open = elevator_proc_open;
	fops.read = elevator_proc_read;
	fops.release = elevator_proc_release;
	
	// assign functions to function pointers for sys calls
	STUB_start_elevator = start_elevator;
	STUB_issue_request = issue_request;
	STUB_run_elevator = run_elevator;

	if (!proc_create(ENTRY_NAME, PERMS, NULL, &fops)) {
		printk(KERN_WARNING "proc create\n");
		remove_proc_entry(ENTRY_NAME, NULL);
		return -ENOMEM;
	}

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

static void elevator_exit(void) {
	// set sys call function pointers to NULLs
	STUB_start_elevator = NULL;
	STUB_issue_request = NULL;
	STUB_run_elevator = NULL;

	int elevator_ret = kthread_stop(elevator_thread);
	if (elevator_ret != -EINTR)
		printk("Elevator thread has stopped\n");
	remove_proc_entry(ENTRY_NAME, NULL);
	printk(KERN_NOTICE "Removing /proc/%s\n", ENTRY_NAME);
}
module_exit(elevator_exit);
