#include <glib.h>
#include <string.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "terminal/terminal-regex.h"

static pcre2_code *
compile_pattern (const gchar *pattern)
{
  gint error_number;
  PCRE2_SIZE error_offset;
  pcre2_code *code;

  code = pcre2_compile_8 ((PCRE2_SPTR8) pattern,
                          PCRE2_ZERO_TERMINATED,
                          PCRE2_UTF | PCRE2_UCP | PCRE2_MULTILINE,
                          &error_number,
                          &error_offset,
                          NULL);
  g_assert_nonnull (code);
  return code;
}

static gint
match (pcre2_code *code,
       const gchar *text,
       PCRE2_SIZE *start,
       PCRE2_SIZE *end)
{
  pcre2_match_data *match_data;
  gint result;
  PCRE2_SIZE *ovector;

  match_data = pcre2_match_data_create_from_pattern_8 (code, NULL);
  result = pcre2_match_8 (code,
                          (PCRE2_SPTR8) text,
                          strlen (text),
                          0,
                          0,
                          match_data,
                          NULL);
  if (result >= 0)
    {
      ovector = pcre2_get_ovector_pointer_8 (match_data);
      if (start != NULL)
        *start = ovector[0];
      if (end != NULL)
        *end = ovector[1];
    }
  pcre2_match_data_free_8 (match_data);
  return result;
}

static void
test_url_wrapping (void)
{
  pcre2_code *code = compile_pattern (REGEX_URL_AS_IS);
  const gchar *text = "https://one.example/x\nhttps://two.example/y";
  const gchar *ordinary_text = "https://one.example/x\nordinary next line";
  PCRE2_SIZE start;
  PCRE2_SIZE end;
  PCRE2_SIZE ordinary_start;
  PCRE2_SIZE ordinary_end;

  g_assert_cmpint (match (code, text, &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, 0);
  g_assert_cmpuint (end, ==, strlen ("https://one.example/x"));
  g_assert_cmpint (match (code, ordinary_text, &ordinary_start, &ordinary_end), >=, 0);
  g_assert_cmpuint (ordinary_start, ==, 0);
  g_assert_cmpuint (ordinary_end, ==, strlen ("https://one.example/x"));
  pcre2_code_free (code);
}

static void
test_file_path_fragments (void)
{
  pcre2_code *code = compile_pattern (REGEX_FILE_PATH);
  const gchar *cases[] = {
    "exports/VOLUME.html#combination_922",
    "(exports/VOLUME.html#combination_922)",
    "Read(~/tasks/diet/exports/VOLUME.html#combination_922)",
    "./VOLUME.html#combination_922",
    "VOLUME.html#combination_922",
    "(exports/VOLUME.html#section%20one)",
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      PCRE2_SIZE start, end;
      const gchar *path = strchr (cases[i], '(');
      gsize offset = path != NULL ? path + 1 - cases[i] : 0;
      gsize length = strlen (cases[i]);
      if (cases[i][length - 1] == ')')
        length--;
      g_assert_cmpint (match (code, cases[i], &start, &end), >=, 0);
      g_assert_cmpuint (start, ==, offset);
      g_assert_cmpuint (end, ==, length);
    }
  g_assert_cmpint (match (code, "57216.795612#section", NULL, NULL), ==, PCRE2_ERROR_NOMATCH);
  pcre2_code_free (code);
}

static void
test_file_path_hyphen_wrapping (void)
{
  pcre2_code *code = compile_pattern (REGEX_FILE_PATH);
  const struct { const gchar *text; const gchar *prefix; gsize suffix; } cases[] = {
    { "Audit results (artifacts/anatomy-\n  tree-toggle-audit/verification-summary.json).", "Audit results (", 2 },
    { "Read(~/artifacts/anatomy-\r\n    tree-toggle-audit/verification-summary.json)", "Read(", 1 },
    { "/tmp/artifacts/anatomy-\ntree-toggle-audit/verification-summary.json", "", 0 },
  };
  PCRE2_SIZE start, end;

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_assert_cmpint (match (code, cases[i].text, &start, &end), >=, 0);
      g_assert_cmpuint (start, ==, strlen (cases[i].prefix));
      g_assert_cmpuint (end, ==, strlen (cases[i].text) - cases[i].suffix);
    }

  /* Hyphenated prose and blank paragraphs must not become one path. */
  g_assert_cmpint (match (code, "message-\n  next.json", &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, strlen ("message-\n  "));
  g_assert_cmpint (match (code, "artifacts/anatomy-\n\n  tree-toggle-audit/summary.json", &start, &end), >=, 0);
  g_assert_cmpuint (end, ==, strlen ("artifacts/anatomy-"));
  pcre2_code_free (code);
}

static void
test_file_path_boundaries (void)
{
  pcre2_code *code = compile_pattern (REGEX_FILE_PATH);
  const gchar *wrapped_absolute = "/\nhome/lewis/.local/share/pcmanfm/ui/pref.ui";
  const gchar *wrapped_relative = "src/\npref.c";
  const gchar *wrapped_crlf = "/\r\nhome/lewis/pref.ui";
  const gchar *wrapped_indented = "Read(~/\n    Dev/fsx/README.md)";
  const gchar *mid_component_break = "/home/lew\nis/pref.ui";
  const gchar *incomplete_closing_bracket = "ad6258e7-c84b-470d-93ff-58ec1b3f89cd/task-390)";
  const gchar *wrapped_tool_path = "Read(~/Dev/fsx/README.md)";
  const gchar *wrapped_tool_parentheses = "Read(~/Dev/fsx/file(name).md)";
  const gchar *parenthesized_relative = "(exports/EXACT_ITEM_COUNT.html)";
  const gchar *parenthesized_absolute = "(/home/lewis/tasks/diet/exports/EXACT_ITEM_COUNT.html)";
  const gchar *unclosed_function_expression = "fs.readFileSync(paths.events";
  const gchar *wrapped_tree_path = "324ed101a15ad00132e9664f04bdeb82b08a203e3f6b3c1207d2e688c3c5ec84  experiments/bole-tree-verify/hip/\n    iq4_nl_q16_attention.hip";
  const gchar *scoped_package_path = "~/.local/share/qwen-r9700/pi/0.84.2/node_modules/@earendil-works/pi-c";
  PCRE2_SIZE start;
  PCRE2_SIZE end;

  g_assert_cmpint (match (code, "résumé.cpp --line 20", &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, 0);
  g_assert_cmpuint (end, ==, strlen ("résumé.cpp"));
  g_assert_cmpint (match (code, ".gitignore", NULL, NULL), >=, 0);
  g_assert_cmpint (match (code, ".123", NULL, NULL), ==, PCRE2_ERROR_NOMATCH);
  g_assert_cmpint (match (code, "Makefile", NULL, NULL), >=, 0);
  g_assert_cmpint (match (code, "tests.output_contract.CopyPreviewContractTests.test_non_utf8", NULL, NULL), >=, 0);
  g_assert_cmpint (match (code, "57216.795612", NULL, NULL), ==, PCRE2_ERROR_NOMATCH);
  g_assert_cmpint (match (code, wrapped_absolute, &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, 0);
  g_assert_cmpuint (end, ==, strlen (wrapped_absolute));
  g_assert_cmpint (match (code, wrapped_relative, &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, 0);
  g_assert_cmpuint (end, ==, strlen (wrapped_relative));
  g_assert_cmpint (match (code, wrapped_crlf, &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, 0);
  g_assert_cmpuint (end, ==, strlen (wrapped_crlf));
  g_assert_cmpint (match (code, wrapped_indented, &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, strlen ("Read("));
  g_assert_cmpuint (end, ==, strlen (wrapped_indented) - 1);
  g_assert_cmpint (match (code, mid_component_break, &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, 0);
  g_assert_cmpuint (end, ==, strlen ("/home/lew"));
  g_assert_cmpint (match (code, wrapped_tool_path, &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, strlen ("Read("));
  g_assert_cmpuint (end, ==, strlen (wrapped_tool_path) - 1);
  g_assert_cmpint (match (code, wrapped_tool_parentheses, &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, strlen ("Read("));
  g_assert_cmpuint (end, ==, strlen (wrapped_tool_parentheses) - 1);
  g_assert_cmpint (match (code, parenthesized_relative, &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, 1);
  g_assert_cmpuint (end, ==, strlen (parenthesized_relative) - 1);
  g_assert_cmpint (match (code, parenthesized_absolute, &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, 1);
  g_assert_cmpuint (end, ==, strlen (parenthesized_absolute) - 1);
  g_assert_cmpint (match (code, incomplete_closing_bracket, &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, 0);
  g_assert_cmpuint (end, ==, strlen (incomplete_closing_bracket) - 1);
  g_assert_cmpint (match (code, unclosed_function_expression, NULL, NULL), ==, PCRE2_ERROR_NOMATCH);
  g_assert_cmpint (match (code, wrapped_tree_path, &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, strlen ("324ed101a15ad00132e9664f04bdeb82b08a203e3f6b3c1207d2e688c3c5ec84  "));
  g_assert_cmpuint (end, ==, strlen (wrapped_tree_path));
  g_assert_cmpint (match (code, scoped_package_path, &start, &end), >=, 0);
  g_assert_cmpuint (start, ==, 0);
  g_assert_cmpuint (end, ==, strlen (scoped_package_path));
  pcre2_code_free (code);
}

int
main (int argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/regex/url/wrapped-boundary", test_url_wrapping);
  g_test_add_func ("/regex/path/boundaries", test_file_path_boundaries);
  g_test_add_func ("/regex/path/hyphen-wrapping", test_file_path_hyphen_wrapping);
  g_test_add_func ("/regex/path/fragments", test_file_path_fragments);
  return g_test_run ();
}
