<?php

function remove_generated_tree($path)
{
    if (is_link($path) || is_file($path))
    {
        @unlink($path);
        return;
    }
    if (!is_dir($path))
    {
        return;
    }
    foreach (scandir($path) as $entry)
    {
        if ($entry === "." || $entry === "..")
        {
            continue;
        }
        remove_generated_tree($path.DIRECTORY_SEPARATOR.$entry);
    }
    @rmdir($path);
}

function fail_generation($message, $temporary_dir = null)
{
    if ($temporary_dir !== null)
    {
        remove_generated_tree($temporary_dir);
    }
    fwrite(STDERR, $message.PHP_EOL);
    exit(1);
}

function is_absolute_path($path)
{
    return strlen($path) > 0 &&
        ($path[0] === "/" || $path[0] === "\\" ||
         (strlen($path) > 2 && ctype_alpha($path[0]) && $path[1] === ":" &&
          ($path[2] === "/" || $path[2] === "\\")));
}

function prepare_thrift_tool($repo_root, $temporary_dir)
{
    $os_name = explode(" ", php_uname())[0];
    $extension = strtoupper(substr(PHP_OS, 0, 3)) === "WIN" ? ".exe" : "";
    $build_dir = getenv("DSN_BUILD_DIR");
    if ($build_dir === FALSE || $build_dir === "")
    {
        $build_dir = $repo_root.DIRECTORY_SEPARATOR."builder";
    }
    else if (!is_absolute_path($build_dir))
    {
        $build_dir = $repo_root.DIRECTORY_SEPARATOR.$build_dir;
    }

    $installed_candidates = array(
        $build_dir.DIRECTORY_SEPARATOR."output".DIRECTORY_SEPARATOR."bin".
            DIRECTORY_SEPARATOR.$os_name.DIRECTORY_SEPARATOR."thrift".$extension,
        $repo_root.DIRECTORY_SEPARATOR."bin".DIRECTORY_SEPARATOR.$os_name.
            DIRECTORY_SEPARATOR."thrift".$extension
    );
    foreach ($installed_candidates as $candidate)
    {
        if (is_file($candidate) && is_executable($candidate))
        {
            return;
        }
    }

    $build_tree_tool = $build_dir.DIRECTORY_SEPARATOR."ext".DIRECTORY_SEPARATOR."thrift".
        DIRECTORY_SEPARATOR."thrift-prefix".DIRECTORY_SEPARATOR."src".
        DIRECTORY_SEPARATOR."thrift-build".DIRECTORY_SEPARATOR."bin".
        DIRECTORY_SEPARATOR."thrift".$extension;
    if (!is_file($build_tree_tool) || !is_executable($build_tree_tool))
    {
        fail_generation(
            "failed to find an installed or build-tree Thrift compiler", $temporary_dir);
    }

    $shim_root = $temporary_dir.DIRECTORY_SEPARATOR."tool-root";
    $shim_dir = $shim_root.DIRECTORY_SEPARATOR."output".DIRECTORY_SEPARATOR."bin".
        DIRECTORY_SEPARATOR.$os_name;
    if (!mkdir($shim_dir, 0700, true))
    {
        fail_generation("failed to create temporary Thrift tool directory", $temporary_dir);
    }
    $shim_tool = $shim_dir.DIRECTORY_SEPARATOR."thrift".$extension;
    if (!@symlink($build_tree_tool, $shim_tool))
    {
        if (!copy($build_tree_tool, $shim_tool))
        {
            fail_generation(
                "failed to expose the build-tree Thrift compiler", $temporary_dir);
        }
        @chmod($shim_tool, 0700);
    }
    putenv("DSN_BUILD_DIR=".$shim_root);
}

function normalize_runtime_artifact($path, $serialization_adapter)
{
    $content = file_get_contents($path);
    if ($content === FALSE)
    {
        throw new RuntimeException("failed to read generated artifact ".$path);
    }

    $content = str_replace(array("\r\n", "\r"), "\n", $content);
    $content = preg_replace('/[ \t]+$/m', '', $content);
    if ($content === null)
    {
        throw new RuntimeException("failed to normalize whitespace in ".$path);
    }

    if ($serialization_adapter)
    {
        if (substr_count(
                $content, "namespace dsn { namespace rasn { namespace rpc {\n") !== 1 ||
            substr_count($content, "\n} } }") !== 1)
        {
            throw new RuntimeException(
                "generated serialization adapter has an unexpected namespace layout");
        }
        return rtrim($content, "\n");
    }

    return rtrim($content, "\n")."\n";
}

$check_only = false;
if (count($argv) === 2 && $argv[1] === "--check")
{
    $check_only = true;
}
else if (count($argv) !== 1)
{
    fail_generation("usage: php src/plugins/rasn/generate_runtime_rpc.php [--check]");
}

$rasn_dir = __DIR__;
$repo_root = dirname(dirname(dirname($rasn_dir)));
$generator = $repo_root.DIRECTORY_SEPARATOR."bin".DIRECTORY_SEPARATOR."dsn.generate_code.php";
$schema = $rasn_dir.DIRECTORY_SEPARATOR."rasn_runtime.thrift";
$temporary_dir = sys_get_temp_dir().DIRECTORY_SEPARATOR.
    "rasn-runtime-codegen-".getmypid()."-".str_replace(".", "-", uniqid("", true));
$generated_dir = $temporary_dir.DIRECTORY_SEPARATOR."generated";

if (!is_file($generator) || !is_file($schema))
{
    fail_generation("rASN runtime generator inputs are missing");
}
if (!mkdir($temporary_dir, 0700, true))
{
    fail_generation("failed to create temporary generation directory ".$temporary_dir);
}

prepare_thrift_tool($repo_root, $temporary_dir);
$command = escapeshellarg(PHP_BINARY)." ".escapeshellarg($generator)." ".
    escapeshellarg($schema)." cpp ".escapeshellarg($generated_dir)." binary";
passthru($command, $generation_status);
if ($generation_status !== 0)
{
    fail_generation("stock rDSN code generation failed", $temporary_dir);
}

$artifacts = array(
    "rasn_runtime_types.h" => false,
    "rasn_runtime_types.cpp" => false,
    "rasn_runtime_constants.h" => false,
    "rasn_runtime_constants.cpp" => false,
    "rasn_runtime.types.h" => true
);
$normalized = array();
try
{
    foreach ($artifacts as $name => $serialization_adapter)
    {
        $generated = $generated_dir.DIRECTORY_SEPARATOR.$name;
        if (!is_file($generated))
        {
            throw new RuntimeException("stock generator did not produce ".$name);
        }
        $normalized[$name] =
            normalize_runtime_artifact($generated, $serialization_adapter);
    }
}
catch (RuntimeException $error)
{
    fail_generation($error->getMessage(), $temporary_dir);
}

$stale = false;
foreach ($normalized as $name => $content)
{
    $target = $rasn_dir.DIRECTORY_SEPARATOR.$name;
    $current = is_file($target) ? file_get_contents($target) : FALSE;
    if ($current === $content)
    {
        echo $name." is current".PHP_EOL;
        continue;
    }
    if ($check_only)
    {
        fwrite(STDERR, $name." is stale".PHP_EOL);
        $stale = true;
        continue;
    }
    if (file_put_contents($target, $content) === FALSE)
    {
        fail_generation("failed to update ".$target, $temporary_dir);
    }
    echo "updated ".$name.PHP_EOL;
}

remove_generated_tree($temporary_dir);
exit($stale ? 1 : 0);
