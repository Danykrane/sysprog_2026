#include "userfs.h"

#include "rlist.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr size_t BLOCK_SIZE = 512;
constexpr size_t MAX_FILE_SIZE = 1024 * 1024 * 100;

constexpr int OK = 0;
constexpr int ERROR = -1;
constexpr size_t VALUE_EOF = 0;

ufs_error_code g_ufs_errno = UFS_ERR_NO_ERR;

struct block {
	char memory[BLOCK_SIZE]{};
	rlist in_block_list = RLIST_LINK_INITIALIZER;
};

struct file {
	rlist blocks = RLIST_HEAD_INITIALIZER(blocks);
	int refs = 0;
	std::string name;
	rlist in_file_list = RLIST_LINK_INITIALIZER;
	size_t size = 0;
	size_t block_count = 0;
	bool is_deleted = false;
};

rlist file_list = RLIST_HEAD_INITIALIZER(file_list);

struct filedesc {
	file *atfile = nullptr;
	size_t position = 0;
	block *current_block = nullptr;
	size_t current_offset = 0;
#if NEED_OPEN_FLAGS
	int access_flags = UFS_READ_WRITE;
#endif
};

std::vector<filedesc*> file_descriptors;

// найти файл по имени
file *file_find(const char *filename) {
	if (filename == nullptr)
		return nullptr;

	file *it = nullptr;
	rlist_foreach_entry(it, &file_list, in_file_list) {
		if (it->name == filename)
			return it;
	}
	return nullptr;
}

// первый блок файла
block *file_first_block(const file *f) {
	if (rlist_empty(&f->blocks))
		return nullptr;
	return rlist_first_entry(&f->blocks, block, in_block_list);
}

// следующий блок файла
block *file_next_block(const file *f, block *b) {
	if (b->in_block_list.next == &f->blocks)
		return nullptr;
	return rlist_next_entry(b, in_block_list);
}

// блок по индексу
block *file_get_block(const file *f, size_t index) {
	if (index >= f->block_count)
		return nullptr;

	block *cur = file_first_block(f);
	while (index > 0 && cur != nullptr) {
		cur = file_next_block(f, cur);
		--index;
	}
	return cur;
}

// число блоков под размер
size_t blocks_for_size(size_t size) {
	if (size == 0)
		return 0;
	return (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
}

// удалить все блоки файла
void file_drop_all_blocks(file *f) {
	block *it = nullptr;
	block *tmp = nullptr;
	rlist_foreach_entry_safe(it, &f->blocks, in_block_list, tmp) {
		rlist_del_entry(it, in_block_list);
		delete it;
	}
	f->block_count = 0;
	f->size = 0;
}

// удалить объект файла
void file_delete_object(file *f) {
	file_drop_all_blocks(f);
	delete f;
}

// добавить блоки
void file_ensure_blocks(file *f, const size_t count) {
	while (f->block_count < count) {
		auto block_holder = std::make_unique<block>();
		rlist_add_tail_entry(&f->blocks, block_holder.get(), in_block_list);
		block_holder.release();
		++f->block_count;
	}
}

// удалить блоки с конца
void file_truncate_blocks(file *f, size_t count) {
	while (f->block_count > count) {
		block *b = rlist_last_entry(&f->blocks, block, in_block_list);
		rlist_del_entry(b, in_block_list);
		delete b;
		--f->block_count;
	}
}

// занулить диапазон
void file_zero_range(const file *f, size_t from, size_t size) {
	if (size == 0)
		return;

	size_t block_index = from / BLOCK_SIZE;
	size_t block_offset = from % BLOCK_SIZE;
	block *cur = file_get_block(f, block_index);

	while (size > 0 && cur != nullptr) {
		size_t chunk = std::min(size, BLOCK_SIZE - block_offset);
		memset(cur->memory + block_offset, 0, chunk);
		size -= chunk;
		block_offset = 0;
		if (size > 0)
			cur = file_next_block(f, cur);
	}
}

// проверить fd
bool fd_is_valid(const int fd) {
	return fd > 0 &&
		static_cast<size_t>(fd - 1) < file_descriptors.size() &&
		file_descriptors[fd - 1] != nullptr;
}

// переставить курсор
void fd_set_position(filedesc *desc, size_t pos) {
	desc->position = pos;
	desc->current_block = nullptr;
	desc->current_offset = 0;

	file *f = desc->atfile;
	size_t block_index = pos / BLOCK_SIZE;
	size_t block_offset = pos % BLOCK_SIZE;

	if (block_index >= f->block_count)
		return;

	desc->current_block = file_get_block(f, block_index);
	desc->current_offset = block_offset;
}

// снять ссылку с файла
void file_unref(file *f) {
	--f->refs;
	if (f->refs == 0 && f->is_deleted)
		file_delete_object(f);
}

// поправить позиции после shrink
void file_fix_descriptors_after_shrink(const file *f, size_t new_size) {
	for (filedesc *desc : file_descriptors) {
		if (desc == nullptr || desc->atfile != f)
			continue;

		if (desc->position > new_size)
			fd_set_position(desc, new_size);
		else
			fd_set_position(desc, desc->position);
	}
}

} // namespace

ufs_error_code ufs_errno() {
	return g_ufs_errno;
}

int ufs_open(const char *filename, int flags) {
	if (filename == nullptr) {
		g_ufs_errno = UFS_ERR_NO_FILE;
		return ERROR;
	}

	file *f = file_find(filename);
	std::unique_ptr<file> file_holder;
	if (f == nullptr) {
		if ((flags & UFS_CREATE) == 0) {
			g_ufs_errno = UFS_ERR_NO_FILE;
			return ERROR;
		}

		file_holder = std::make_unique<file>();
		file_holder->name = filename;
		f = file_holder.get();
	}

	auto desc_holder = std::make_unique<filedesc>();
	desc_holder->atfile = f;
#if NEED_OPEN_FLAGS
	int mode = flags & UFS_READ_WRITE;
	if (mode == 0)
		mode = UFS_READ_WRITE;
	desc_holder->access_flags = mode;
#else
	(void)flags;
#endif
	fd_set_position(desc_holder.get(), 0);

	size_t index = 0;
	while (index < file_descriptors.size() && file_descriptors[index] != nullptr)
		++index;

	if (index == file_descriptors.size())
		file_descriptors.push_back(nullptr);

	file_descriptors[index] = desc_holder.get();

	// добавить новый файл в список только после успеха
	if (file_holder != nullptr) {
		rlist_add_tail_entry(&file_list, file_holder.get(), in_file_list);
		file_holder.release();
	}

	desc_holder.release();

	++f->refs;
	g_ufs_errno = UFS_ERR_NO_ERR;
	return static_cast<int>(index + 1);
}

ssize_t ufs_write(const int fd, const char *buf, const size_t size) {
	if (!fd_is_valid(fd)) {
		g_ufs_errno = UFS_ERR_NO_FILE;
		return ERROR;
	}

	filedesc *desc = file_descriptors[fd - 1];
#if NEED_OPEN_FLAGS
	if ((desc->access_flags & UFS_WRITE_ONLY) == 0) {
		g_ufs_errno = UFS_ERR_NO_PERMISSION;
		return ERROR;
	}
#endif

	if (size == 0) {
		g_ufs_errno = UFS_ERR_NO_ERR;
		return OK;
	}

	file *f = desc->atfile;
	if (desc->position > MAX_FILE_SIZE || size > MAX_FILE_SIZE - desc->position) {
		g_ufs_errno = UFS_ERR_NO_MEM;
		return ERROR;
	}

	size_t end_pos = desc->position + size;
	size_t need_blocks = blocks_for_size(end_pos);
	file_ensure_blocks(f, need_blocks);

	if (desc->current_block == nullptr)
		fd_set_position(desc, desc->position);

	block *cur = desc->current_block;
	size_t off = desc->current_offset;
	const char *src = buf;
	size_t left = size;

	while (left > 0 && cur != nullptr) {
		size_t chunk = std::min(left, BLOCK_SIZE - off);
		memcpy(cur->memory + off, src, chunk);
		src += chunk;
		left -= chunk;
		desc->position += chunk;
		off += chunk;

		if (off == BLOCK_SIZE) {
			cur = file_next_block(f, cur);
			off = 0;
		}
	}

	desc->current_block = cur;
	desc->current_offset = off;

	if (end_pos > f->size)
		f->size = end_pos;

	g_ufs_errno = UFS_ERR_NO_ERR;
	return static_cast<ssize_t>(size);
}

ssize_t ufs_read(int fd, char *buf, size_t size) {
	if (!fd_is_valid(fd)) {
		g_ufs_errno = UFS_ERR_NO_FILE;
		return ERROR;
	}

	filedesc *desc = file_descriptors[fd - 1];
#if NEED_OPEN_FLAGS
	if ((desc->access_flags & UFS_READ_ONLY) == 0) {
		g_ufs_errno = UFS_ERR_NO_PERMISSION;
		return ERROR;
	}
#endif

	file *f = desc->atfile;
	if (desc->position >= f->size) {
		g_ufs_errno = UFS_ERR_NO_ERR;
		return VALUE_EOF;
	}

	size_t to_read = std::min(size, f->size - desc->position);
	if (to_read == 0) {
		g_ufs_errno = UFS_ERR_NO_ERR;
		return VALUE_EOF;
	}

	if (desc->current_block == nullptr)
		fd_set_position(desc, desc->position);

	block *cur = desc->current_block;
	size_t off = desc->current_offset;
	char *dst = buf;
	size_t left = to_read;

	while (left > 0 && cur != nullptr) {
		size_t chunk = std::min(left, BLOCK_SIZE - off);
		memcpy(dst, cur->memory + off, chunk);
		dst += chunk;
		left -= chunk;
		desc->position += chunk;
		off += chunk;

		if (off == BLOCK_SIZE) {
			cur = file_next_block(f, cur);
			off = 0;
		}
	}

	desc->current_block = cur;
	desc->current_offset = off;

	g_ufs_errno = UFS_ERR_NO_ERR;
	return static_cast<ssize_t>(to_read);
}

int ufs_close(int fd) {
	if (!fd_is_valid(fd)) {
		g_ufs_errno = UFS_ERR_NO_FILE;
		return ERROR;
	}

	filedesc *desc = file_descriptors[fd - 1];
	file *f = desc->atfile;
	delete desc;
	file_descriptors[fd - 1] = nullptr;
	file_unref(f);

	g_ufs_errno = UFS_ERR_NO_ERR;
	return OK;
}

int ufs_delete(const char *filename) {
	file *f = file_find(filename);
	if (f == nullptr) {
		g_ufs_errno = UFS_ERR_NO_FILE;
		return ERROR;
	}

	rlist_del_entry(f, in_file_list);
	f->is_deleted = true;

	if (f->refs == 0)
		file_delete_object(f);

	g_ufs_errno = UFS_ERR_NO_ERR;
	return OK;
}

#if NEED_RESIZE
int ufs_resize(int fd, size_t new_size) {
	if (!fd_is_valid(fd)) {
		g_ufs_errno = UFS_ERR_NO_FILE;
		return ERROR;
	}

	filedesc *desc = file_descriptors[fd - 1];
#if NEED_OPEN_FLAGS
	if ((desc->access_flags & UFS_WRITE_ONLY) == 0) {
		g_ufs_errno = UFS_ERR_NO_PERMISSION;
		return ERROR;
	}
#endif

	if (new_size > MAX_FILE_SIZE) {
		g_ufs_errno = UFS_ERR_NO_MEM;
		return ERROR;
	}

	file *f = desc->atfile;
	size_t old_size = f->size;

	if (new_size > old_size) {
		size_t need_blocks = blocks_for_size(new_size);
		file_ensure_blocks(f, need_blocks);
		file_zero_range(f, old_size, new_size - old_size);
		f->size = new_size;
		g_ufs_errno = UFS_ERR_NO_ERR;
		return OK;
	}

	if (new_size < old_size) {
		size_t keep_blocks = blocks_for_size(new_size);
		file_truncate_blocks(f, keep_blocks);
		f->size = new_size;
		file_fix_descriptors_after_shrink(f, new_size);
		g_ufs_errno = UFS_ERR_NO_ERR;
		return OK;
	}

	g_ufs_errno = UFS_ERR_NO_ERR;
	return OK;
}
#endif

void ufs_destroy() {
	for (size_t i = 0; i < file_descriptors.size(); ++i) {
		if (file_descriptors[i] != nullptr)
			ufs_close(static_cast<int>(i + 1));
	}

	file *it = nullptr;
	file *tmp = nullptr;
	rlist_foreach_entry_safe(it, &file_list, in_file_list, tmp) {
		rlist_del_entry(it, in_file_list);
		file_delete_object(it);
	}

	std::vector<filedesc*> empty;
	file_descriptors.swap(empty);
}