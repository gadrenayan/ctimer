/*mytimer.c*/

#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/timerfd.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <assert.h>
#include <errno.h>

#include "ctimer.h"
#include "btree/btree.h"

#define MAX_TIMER_COUNT 1000

struct timer_node
{
	int                 fd;
	time_handler        callback;
	void *              user_data;
	unsigned int        interval;
	t_timer             type;
};

static void * _timer_thread(void * data);
static pthread_t g_thread_id;
static struct btree *g_root = NULL;
int epoll_fd = -1;

int compare_fd(const void *a, const void *b, void *udata) {
	const struct timer_node *ua = a;
	const struct timer_node *ub = b;

	if (ua->fd == ub->fd) {
		return 0;
	} else if (ua->fd > ub->fd) {
		return 1;
	} else {
		return -1;
	}
}

void timer_node_free(const void *item, void *udata) {
	struct timer_node *node = (struct timer_node*)item;
	struct epoll_event event;
	printf("%s: called for fd: %d interval: %u\n", __func__, node->fd, node->interval);
	memset(&event, 0 , sizeof(struct epoll_event));
	int ret = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, node->fd, NULL);
	if (ret != 0) {
		printf("%s: Failed to DEL fd: %d timer: %u\n", __func__, node->fd, node->interval);
		assert(0);
	}
	close(node->fd);
	free(node);
	printf("%s: freed node\n", __func__);
}

int initialize()
{
	printf("%s\n", __func__);
	g_root = btree_new(sizeof(struct timer_node), 0, compare_fd, NULL);
	if (g_root == NULL) {
		printf("%s: couldn't init btree root\n", __func__);
		assert(0);
	}
	btree_set_item_callbacks(g_root, NULL, timer_node_free);
	if(pthread_create(&g_thread_id, NULL, _timer_thread, NULL))
	{
		/*Thread creation failed*/
		return 0;
	}
	return 1;
}

size_t start_timer(unsigned int interval, time_handler handler, t_timer type, void * user_data)
{
	struct timer_node * new_node = NULL;
	struct itimerspec new_value;

	printf("%s\n", __func__);
	new_node = (struct timer_node *)malloc(sizeof(struct timer_node));

	if(new_node == NULL) return 0;

	new_node->callback  = handler;
	new_node->user_data = user_data;
	new_node->interval  = interval;
	new_node->type      = type;

	new_node->fd = timerfd_create(CLOCK_REALTIME, 0);

	if (new_node->fd == -1)
	{
		free(new_node);
		return 0;
	}

	new_value.it_value.tv_sec = interval / 1000;
	new_value.it_value.tv_nsec = (interval % 1000)* 1000000;

	if (type == TIMER_PERIODIC) {
		new_value.it_interval.tv_sec= interval / 1000;
		new_value.it_interval.tv_nsec = (interval %1000) * 1000000;
	} else {
		new_value.it_interval.tv_sec= 0;
		new_value.it_interval.tv_nsec = 0;
	}

	timerfd_settime(new_node->fd, 0, &new_value, NULL);

	/*Inserting the timer node into the btree*/
	void * ret = btree_set(g_root, new_node);
	if (ret != NULL) {
		printf("%s: btree_set failed for fd: %d interval: %u\n", __func__, new_node->fd, interval);
		assert(0);
	}

	struct epoll_event event;
	memset(&event, 0, sizeof(struct epoll_event));

	event.events = EPOLLIN;
	event.data.fd = new_node->fd;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_node->fd, &event)) {
		printf("Failed to add file descriptor to epoll: %s\n", strerror(errno));
		close(epoll_fd);
		assert(0);
	}

	return (size_t)new_node;
}

void stop_timer(size_t timer_id) {
	struct timer_node * tmp = NULL;
	struct timer_node * node = (struct timer_node *)timer_id;

	if (node == NULL) return;
	printf("%s\n", __func__);
/*
	struct epoll_event event;
	memset(&event, 0 , sizeof(struct epoll_event));
	int ret = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, node->fd, NULL);
	if (ret != 0) {
		printf("%s: Failed to DEL fd: %d timer: %u\n", __func__, node->fd, node->interval);
		assert(0);
	}
*/
	void *ret = btree_delete(g_root, node);
	if (ret == NULL) {
		printf("%s: Could not find node fd: %d interval: %u\n", __func__, node->fd, node->interval);
		assert(0);
	}
/*
	close(node->fd);
	free(node);
*/
}

void finalize()
{
	printf("%s\n", __func__);
	btree_free(g_root);
	pthread_cancel(g_thread_id);
	pthread_join(g_thread_id, NULL);
}
/*
struct timer_node * _get_timer_from_fd(int fd)
{
	struct timer_node * tmp = g_head;

	while(tmp)
	{
		if(tmp->fd == fd) return tmp;

		tmp = tmp->next;
	}
	return NULL;
}
*/
void * _timer_thread(void * data)
{
	struct epoll_event events[MAX_TIMER_COUNT];
	struct timer_node * tmp = NULL;
	int read_fds = 0, i, s, event_count;
	uint64_t exp;
	int running = 1;
	memset(events, 0, sizeof(struct epoll_event) * MAX_TIMER_COUNT);
	
	epoll_fd = epoll_create1(0);
	if (epoll_fd == -1) {
		printf("Failed to create epoll file descriptor\n");
		return NULL;
	}
	printf("%s: Created Epoll context\n", __func__);
/*
	tmp = g_head;
	while(tmp)
	{
		struct epoll_event event;
		event.events = EPOLLIN;
		event.data.fd = tmp->fd;
		if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tmp->fd, &event))
		{
			printf("Failed to add file descriptor to epoll\n");
			close(epoll_fd);
			return NULL;
		}
		tmp = tmp->next;
	}
*/
	while(running == 1)
	{
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		pthread_testcancel();
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		printf("%s: Started epoll_wait\n", __func__);
		event_count = epoll_wait(epoll_fd, events, MAX_TIMER_COUNT, -1);
		printf("%d events READY\n", event_count);

		for (i = 0; i < event_count; i++)
		{
			printf("Reading the fd: %d\n", events[i].data.fd);
			s = read(events[i].data.fd, &exp, sizeof(uint64_t));
			if (s != sizeof(uint64_t)) continue;
			//tmp = _get_timer_from_fd(events[i].data.fd);
			struct timer_node in = {.fd = events[i].data.fd};
			struct timer_node *out = NULL;
			out = btree_get(g_root, &in);
			if (out == NULL) {
				printf("%s: Could not find timer with fd: %d interval: %u\n", __func__, out->fd, out->interval);
				continue;
			}
			printf("Timer with interval: %u expired\n", out->interval);
			if(out && out->callback) out->callback((size_t)out, out->user_data);
		}
	}
	return NULL;
}
