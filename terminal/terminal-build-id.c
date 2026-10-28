#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "terminal-build-id.h"

static guint64
read_integer (const guint8 *bytes,
              guint size,
              gboolean little_endian)
{
  guint64 value = 0;

  for (guint i = 0; i < size; i++)
    value = (value << 8) | bytes[little_endian ? size - i - 1 : i];
  return value;
}

static gboolean
read_at (gint fd,
         guint8 *bytes,
         gsize size,
         guint64 offset)
{
  while (size > 0)
    {
      ssize_t count = pread (fd, bytes, size, (off_t) offset);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        return FALSE;
      bytes += count;
      size -= count;
      offset += count;
    }
  return TRUE;
}

static gchar *
read_notes (gint fd,
            guint64 offset,
            guint64 size,
            gboolean little_endian)
{
  guint8 *bytes;
  gchar *id = NULL;

  /* Ordinary build-ID notes are tiny. Bound malformed/untrusted local files. */
  if (size < 12 || size > 65536)
    return NULL;
  bytes = g_malloc (size);
  if (!read_at (fd, bytes, size, offset))
    goto out;
  for (guint64 pos = 0; pos + 12 <= size;)
    {
      guint64 namesz = read_integer (bytes + pos, 4, little_endian);
      guint64 descsz = read_integer (bytes + pos + 4, 4, little_endian);
      guint64 type = read_integer (bytes + pos + 8, 4, little_endian);
      guint64 name_space = (namesz + 3) & ~G_GUINT64_CONSTANT (3);
      guint64 desc_space = (descsz + 3) & ~G_GUINT64_CONSTANT (3);

      pos += 12;
      if (name_space > size - pos || desc_space > size - pos - name_space)
        break;
      if (type == 3 && namesz == 4 && memcmp (bytes + pos, "GNU\0", 4) == 0
          && descsz > 0 && descsz <= 256)
        {
          id = g_malloc (descsz * 2 + 1);
          for (guint64 i = 0; i < descsz; i++)
            g_snprintf (id + i * 2, 3, "%02x", bytes[pos + name_space + i]);
          break;
        }
      pos += name_space + desc_space;
    }
out:
  g_free (bytes);
  return id;
}

gchar *
terminal_build_id_read (const gchar *path)
{
  guint8 header[64], program[56];
  struct stat info, after;
  guint64 phoff, phsize, phnum;
  gboolean is64, little_endian;
  gchar *id = NULL;
  gint fd = g_open (path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);

  if (fd < 0)
    return NULL;
  if (fstat (fd, &info) != 0 || !S_ISREG (info.st_mode)
      || info.st_size < 64 || !read_at (fd, header, sizeof header, 0)
      || memcmp (header, "\177ELF", 4) != 0
      || (header[4] != 1 && header[4] != 2)
      || (header[5] != 1 && header[5] != 2) || header[6] != 1)
    goto out;
  is64 = header[4] == 2;
  little_endian = header[5] == 1;
  if (read_integer (header + 20, 4, little_endian) != 1
      || read_integer (header + (is64 ? 52 : 40), 2, little_endian) != (is64 ? 64 : 52))
    goto out;
  if (read_integer (header + 16, 2, little_endian) != 2
      && read_integer (header + 16, 2, little_endian) != 3)
    goto out;
  phoff = read_integer (header + (is64 ? 32 : 28), is64 ? 8 : 4, little_endian);
  phsize = read_integer (header + (is64 ? 54 : 42), 2, little_endian);
  phnum = read_integer (header + (is64 ? 56 : 44), 2, little_endian);
  if (phoff < (is64 ? 64 : 52) || phsize != (is64 ? 56 : 32) || phnum == 0 || phnum > 256
      || phoff > (guint64) info.st_size || phnum * phsize > (guint64) info.st_size - phoff)
    goto out;

  for (guint i = 0; i < phnum; i++)
    {
      guint64 type, offset, size;

      if (!read_at (fd, program, phsize, phoff + i * phsize))
        goto invalid;
      type = read_integer (program, 4, little_endian);
      offset = read_integer (program + (is64 ? 8 : 4), is64 ? 8 : 4, little_endian);
      size = read_integer (program + (is64 ? 32 : 16), is64 ? 8 : 4, little_endian);
      if (offset > (guint64) info.st_size || size > (guint64) info.st_size - offset)
        goto invalid;
      if (type == 4 && id == NULL)
        id = read_notes (fd, offset, size, little_endian);
    }
  if (fstat (fd, &after) != 0 || info.st_size != after.st_size
      || info.st_mtime != after.st_mtime || info.st_ctime != after.st_ctime)
    goto invalid;
#ifdef __linux__
  if (info.st_mtim.tv_nsec != after.st_mtim.tv_nsec
      || info.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
    goto invalid;
#endif
  goto out;
invalid:
  g_clear_pointer (&id, g_free);
out:
  close (fd);
  return id;
}

static const gchar *
find_title_label (const gchar *title,
                  const gchar *label)
{
  gsize title_length = strlen (title), label_length = strlen (label);
  const gchar *suffix;

  if (title_length >= label_length && g_str_has_suffix (title, label))
    {
      suffix = title + title_length - label_length;
      if ((suffix - title >= 3 && strncmp (suffix - 3, " - ", 3) == 0)
          || (suffix - title == 5 && strncmp (title, "Xfce ", 5) == 0))
        return suffix;
    }
  if (g_str_has_prefix (title, label)
      && (title_length == label_length || g_str_has_prefix (title + label_length, " - ")))
    return title;
  return NULL;
}

gchar *
terminal_build_title_format (const gchar *title,
                             const gchar *label,
                             gboolean stale)
{
  GString *struck;
  gchar *formatted;
  const gchar *position;

  if (!stale)
    return g_strdup (title);
  struck = g_string_new (NULL);
  for (const gchar *p = label; *p != '\0'; p = g_utf8_next_char (p))
    {
      g_string_append_unichar (struck, g_utf8_get_char (p));
      g_string_append (struck, "\314\266"); /* U+0336, plain-title strikethrough. */
    }
  position = find_title_label (title, label);
  if (find_title_label (title, struck->str) != NULL)
    formatted = g_strdup (title);
  else if (position != NULL)
    {
      GString *result = g_string_new_len (title, position - title);
      g_string_append (result, struck->str);
      g_string_append (result, position + strlen (label));
      formatted = g_string_free (result, FALSE);
    }
  else
    formatted = g_strconcat (title, " - ", struck->str, NULL);
  g_string_free (struck, TRUE);
  return formatted;
}
