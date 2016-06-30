/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2016  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "general.h"
#include "target.h"
#include "target_internal.h"

target *target_list = NULL;

target *target_new(void)
{
	target *t = (void*)calloc(1, sizeof(*t));
	t->next = target_list;
	target_list = t;

	return t;
}

bool target_foreach(void (*cb)(int, target *t, void *context), void *context)
{
	int i = 1;
	target *t = target_list;
	for (; t; t = t->next, i++)
		cb(i, t, context);
	return target_list != NULL;
}

void target_list_free(void)
{
	struct target_command_s *tc;

	while(target_list) {
		target *t = target_list->next;
		if (target_list->destroy_callback)
			target_list->destroy_callback(target_list);
		if (target_list->priv)
			target_list->priv_free(target_list->priv);
		while (target_list->commands) {
			tc = target_list->commands->next;
			free(target_list->commands);
			target_list->commands = tc;
		}
		if (target_list->dyn_mem_map)
			free(target_list->dyn_mem_map);
		while (target_list->ram) {
			void * next = target_list->ram->next;
			free(target_list->ram);
			target_list->ram = next;
		}
		while (target_list->flash) {
			void * next = target_list->flash->next;
			if (target_list->flash->buf)
				free(target_list->flash->buf);
			free(target_list->flash);
			target_list->flash = next;
		}
		free(target_list);
		target_list = t;
	}
}

void target_add_commands(target *t, const struct command_s *cmds, const char *name)
{
	struct target_command_s *tc;
	if (t->commands) {
		for (tc = t->commands; tc->next; tc = tc->next);
		tc = tc->next = malloc(sizeof(*tc));
	} else {
		t->commands = tc = malloc(sizeof(*tc));
	}
	tc->specific_name = name;
	tc->cmds = cmds;
	tc->next = NULL;
}

target *target_attach_n(int n, target_destroy_callback destroy_cb)
{
	target *t;
	int i;
	for(t = target_list, i = 1; t; t = t->next, i++)
		if(i == n)
			return target_attach(t, destroy_cb);
	return NULL;
}

target *target_attach(target *t, target_destroy_callback destroy_cb)
{
	if (t->destroy_callback)
		t->destroy_callback(t);

	t->destroy_callback = destroy_cb;

	if (!t->attach(t))
		return NULL;

	t->attached = true;
	return t;
}

void target_add_ram(target *t, uint32_t start, uint32_t len)
{
	struct target_ram *ram = malloc(sizeof(*ram));
	ram->start = start;
	ram->length = len;
	ram->next = t->ram;
	t->ram = ram;
}

void target_add_flash(target *t, struct target_flash *f)
{
	f->t = t;
	f->next = t->flash;
	t->flash = f;
}

static ssize_t map_ram(char *buf, size_t len, struct target_ram *ram)
{
	return snprintf(buf, len, "<memory type=\"ram\" start=\"0x%08"PRIx32
	                          "\" length=\"0x%08"PRIx32"\"/>",
	                          ram->start, ram->length);
}

static ssize_t map_flash(char *buf, size_t len, struct target_flash *f)
{
	int i = 0;
	i += snprintf(&buf[i], len - i, "<memory type=\"flash\" start=\"0x%08"PRIx32
	                                "\" length=\"0x%08"PRIx32"\">",
	                                f->start, f->length);
	i += snprintf(&buf[i], len - i, "<property name=\"blocksize\">0x%08"PRIx32
	                            "</property></memory>",
	                            f->blocksize);
	return i;
}

const char *target_mem_map(target *t)
{
	if (t->dyn_mem_map)
		return t->dyn_mem_map;

	/* FIXME size buffer */
	size_t len = 1024;
	char *tmp = malloc(len);
	size_t i = 0;
	i = snprintf(&tmp[i], len - i, "<memory-map>");
	/* Map each defined RAM */
	for (struct target_ram *r = t->ram; r; r = r->next)
		i += map_ram(&tmp[i], len - i, r);
	/* Map each defined Flash */
	for (struct target_flash *f = t->flash; f; f = f->next)
		i += map_flash(&tmp[i], len - i, f);
	i += snprintf(&tmp[i], len - i, "</memory-map>");

	t->dyn_mem_map = tmp;

	return t->dyn_mem_map;
}

static struct target_flash *flash_for_addr(target *t, uint32_t addr)
{
	for (struct target_flash *f = t->flash; f; f = f->next)
		if ((f->start <= addr) &&
		    (addr < (f->start + f->length)))
			return f;
	return NULL;
}

int target_flash_erase(target *t, uint32_t addr, size_t len)
{
	int ret = 0;
	while (len) {
		struct target_flash *f = flash_for_addr(t, addr);
		size_t tmplen = MIN(len, f->length - (addr % f->length));
		ret |= f->erase(f, addr, tmplen);
		addr += tmplen;
		len -= tmplen;
	}
	return ret;
}

int target_flash_write(target *t,
                       uint32_t dest, const void *src, size_t len)
{
	int ret = 0;
	while (len) {
		struct target_flash *f = flash_for_addr(t, dest);
		size_t tmplen = MIN(len, f->length - (dest % f->length));
		if (f->align > 1) {
			uint32_t offset = dest % f->align;
			uint8_t data[ALIGN(offset + len, f->align)];
			memset(data, f->erased, sizeof(data));
			memcpy((uint8_t *)data + offset, src, len);
			ret |= f->write(f, dest - offset, data, sizeof(data));
		} else {
			ret |= f->write(f, dest, src, tmplen);
		}
		src += tmplen;
		len -= tmplen;
	}
	return ret;
}

int target_flash_done(target *t)
{
	for (struct target_flash *f = t->flash; f; f = f->next) {
		if (f->done) {
			int tmp = f->done(f);
			if (tmp)
				return tmp;
		}
	}
	return 0;
}

int target_flash_write_buffered(struct target_flash *f,
                                uint32_t dest, const void *src, size_t len)
{
	int ret = 0;

	if (f->buf == NULL) {
		/* Allocate flash sector buffer */
		f->buf = malloc(f->buf_size);
		f->buf_addr = -1;
	}
	while (len) {
		uint32_t offset = dest % f->buf_size;
		uint32_t base = dest - offset;
		if (base != f->buf_addr) {
			if (f->buf_addr != (uint32_t)-1) {
				/* Write sector to flash if valid */
				ret |= f->write_buf(f, f->buf_addr,
				                    f->buf, f->buf_size);
			}
			/* Setup buffer for a new sector */
			f->buf_addr = base;
			memset(f->buf, f->erased, f->buf_size);
		}
		/* Copy chunk into sector buffer */
		size_t sectlen = MIN(f->buf_size - offset, len);
		memcpy(f->buf + offset, src, sectlen);
		dest += sectlen;
		src += sectlen;
		len -= sectlen;
	}
	return ret;
}

int target_flash_done_buffered(struct target_flash *f)
{
	int ret = 0;
	if ((f->buf != NULL) &&(f->buf_addr != (uint32_t)-1)) {
		/* Write sector to flash if valid */
		ret = f->write_buf(f, f->buf_addr, f->buf, f->buf_size);
		f->buf_addr = -1;
		free(f->buf);
		f->buf = NULL;
	}

	return ret;
}

/* Wrapper functions */
void target_detach(target *t)
{
	t->detach(t);
	t->attached = false;
}

bool target_check_error(target *t) { return t->check_error(t); }
bool target_attached(target *t) { return t->attached; }

/* Memory access functions */
void target_mem_read(target *t, void *dest, uint32_t src, size_t len)
{
	t->mem_read(t, dest, src, len);
}

void target_mem_write(target *t, uint32_t dest, const void *src, size_t len)
{
	t->mem_write(t, dest, src, len);
}

/* Register access functions */
void target_regs_read(target *t, void *data) { t->regs_read(t, data); }
void target_regs_write(target *t, const void *data) { t->regs_write(t, data); }

/* Halt/resume functions */
void target_reset(target *t) { t->reset(t); }
void target_halt_request(target *t) { t->halt_request(t); }
int target_halt_wait(target *t) { return t->halt_wait(t); }
void target_halt_resume(target *t, bool step) { t->halt_resume(t, step); }

/* Break-/watchpoint functions */
int target_set_hw_bp(target *t, uint32_t addr, uint8_t len)
{
	if (t->set_hw_bp == NULL)
		return 0;
	return t->set_hw_bp(t, addr, len);
}

int target_clear_hw_bp(target *t, uint32_t addr, uint8_t len)
{
	if (t->clear_hw_bp == NULL)
		return 0;
	return t->clear_hw_bp(t, addr, len);
}

int target_set_hw_wp(target *t, uint8_t type, uint32_t addr, uint8_t len)
{
	if (t->set_hw_wp == NULL)
		return 0;
	return t->set_hw_wp(t, type, addr, len);
}

int target_clear_hw_wp(target *t, uint8_t type, uint32_t addr, uint8_t len)
{
	if (t->clear_hw_wp == NULL)
		return 0;
	return t->clear_hw_wp(t, type, addr, len);
}

int target_check_hw_wp(target *t, uint32_t *addr)
{
	if (t->check_hw_wp == NULL)
		return 0;
	return t->check_hw_wp(t, addr);
}

/* Host I/O */
void target_hostio_reply(target *t, int32_t retcode, uint32_t errcode)
{
	t->hostio_reply(t, retcode, errcode);
}

/* Accessor functions */
int target_regs_size(target *t)
{
	return t->regs_size;
}

const char *target_tdesc(target *t)
{
	return t->tdesc ? t->tdesc : "";
}

const char *target_driver_name(target *t)
{
	return t->driver;
}

uint32_t target_mem_read32(target *t, uint32_t addr)
{
	uint32_t ret;
	target_mem_read(t, &ret, addr, sizeof(ret));
	return ret;
}

void target_mem_write32(target *t, uint32_t addr, uint32_t value)
{
	target_mem_write(t, addr, &value, sizeof(value));
}

uint16_t target_mem_read16(target *t, uint32_t addr)
{
	uint16_t ret;
	target_mem_read(t, &ret, addr, sizeof(ret));
	return ret;
}

void target_mem_write16(target *t, uint32_t addr, uint16_t value)
{
	target_mem_write(t, addr, &value, sizeof(value));
}

uint8_t target_mem_read8(target *t, uint32_t addr)
{
	uint8_t ret;
	target_mem_read(t, &ret, addr, sizeof(ret));
	return ret;
}

void target_mem_write8(target *t, uint32_t addr, uint8_t value)
{
	target_mem_write(t, addr, &value, sizeof(value));
}

void target_command_help(target *t)
{
	for (struct target_command_s *tc = t->commands; tc; tc = tc->next) {
		tc_printf(t, "%s specific commands:\n", tc->specific_name);
		for(const struct command_s *c = tc->cmds; c->cmd; c++)
			tc_printf(t, "\t%s -- %s\n", c->cmd, c->help);
	}
}

int target_command(target *t, int argc, const char *argv[])
{
	for (struct target_command_s *tc = t->commands; tc; tc = tc->next)
		for(const struct command_s *c = tc->cmds; c->cmd; c++)
			if(!strncmp(argv[0], c->cmd, strlen(argv[0])))
				return !c->handler(t, argc, argv);
	return -1;
}

#include "gdb_packet.h"
void tc_printf(target *t, const char *fmt, ...)
{
	(void)t;
	va_list ap;
	va_start(ap, fmt);
	gdb_voutf(fmt, ap);
	va_end(ap);
}

