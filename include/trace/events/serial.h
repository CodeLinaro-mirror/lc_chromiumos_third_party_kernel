/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM serial

#if !defined(_TRACE_SERIAL_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SERIAL_H

#include <linux/ktime.h>
#include <linux/tracepoint.h>

TRACE_EVENT(serial_info,

	TP_PROTO(const char *func, char *name, unsigned int val),

	TP_ARGS(func, name, val),

	TP_STRUCT__entry(
		__string(func, func)
		__string(name, name)
		__field(unsigned int, val)
	),

	TP_fast_assign(
		__assign_str(func, func);
		__assign_str(name, name);
		__entry->val = val;
	),

	TP_printk("%s:%s: 0x%x", __get_str(func), __get_str(name),
		(unsigned int)__entry->val)

);

TRACE_EVENT(serial_termios,

	TP_PROTO(const char *func, char *name1, unsigned int val1, char *name2,
		unsigned int val2, char *name3, unsigned int val3, char *name4,
		unsigned int val4, char *name5, unsigned int val5),

	TP_ARGS(func, name1, val1, name2, val2, name3, val3, name4, val4,
		name5, val5),

	TP_STRUCT__entry(
		__string(func, func)
		__string(name1, name1)
		__field(unsigned int, val1)
		__string(name2, name2)
		__field(unsigned int, val2)
		__string(name3, name3)
		__field(unsigned int, val3)
		__string(name4, name4)
		__field(unsigned int, val4)
		__string(name5, name5)
		__field(unsigned int, val5)
	),

	TP_fast_assign(
		__assign_str(func, func);
		__assign_str(name1, name1);
		__entry->val1 = val1;
		__assign_str(name2, name2);
		__entry->val2 = val2;
		__assign_str(name3, name3);
		__entry->val3 = val3;
		__assign_str(name4, name4);
		__entry->val4 = val4;
		__assign_str(name5, name5);
		__entry->val5 = val5;
	),

	TP_printk("%s: %s:%d, %s:0x%x, %s:0x%x, %s:0x%x, %s:0x%x",
		__get_str(func), __get_str(name1), (unsigned int)__entry->val1,
		__get_str(name2), (unsigned int)__entry->val2, __get_str(name3),
		(unsigned int)__entry->val3, __get_str(name4),
		(unsigned int)__entry->val4, __get_str(name5),
		(unsigned int)__entry->val5)

);

DECLARE_EVENT_CLASS(serial_transmit_data,

	TP_PROTO(char *string, int size),

	TP_ARGS(string, size),

	TP_STRUCT__entry(
		__array(char, buf, 64)
		__field(unsigned int, size)
		__field(int, len)
	),

	TP_fast_assign(
		__entry->len = min(32, size);
		hex_dump_to_buffer(string, __entry->len, 32, 1, __entry->buf,
			sizeof(__entry->buf), false);
	),

	TP_printk("%s\n", __entry->buf)

);

DEFINE_EVENT(serial_transmit_data, serial_transmit_data_tx,

	TP_PROTO(char *string, int size),

	TP_ARGS(string, size)

);

DEFINE_EVENT(serial_transmit_data, serial_transmit_data_rx,

	TP_PROTO(char *string, int size),

	TP_ARGS(string, size)

);

#endif /* _TRACE_POWER_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
