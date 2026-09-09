// collect_licenses copies license material for every non-main module linked
// through the product's `go list -deps -json .` package graph and writes a
// path-free module stream plus the package publisher's index.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

type module struct {
	Path     string  `json:"Path"`
	Version  string  `json:"Version"`
	Sum      string  `json:"Sum"`
	GoModSum string  `json:"GoModSum"`
	Main     bool    `json:"Main"`
	Dir      string  `json:"Dir"`
	Replace  *module `json:"Replace"`
}

type normalizedModule struct {
	Path     string            `json:"Path"`
	Version  string            `json:"Version,omitempty"`
	Sum      string            `json:"Sum,omitempty"`
	GoModSum string            `json:"GoModSum,omitempty"`
	Main     bool              `json:"Main,omitempty"`
	Replace  *normalizedModule `json:"Replace,omitempty"`
}

type packageMetadata struct {
	Module *module `json:"Module"`
}

type licenseFile struct {
	Path   string `json:"path"`
	SHA256 string `json:"sha256"`
}

type manifestEntry struct {
	Module       string        `json:"module"`
	Version      string        `json:"version"`
	LicenseFiles []licenseFile `json:"license_files"`
}

func main() {
	packagesPath := flag.String("packages", "", "go list -deps -json package stream")
	normalizedPath := flag.String("normalized", "", "normalized module JSON stream")
	outputRoot := flag.String("output", "", "runtime legal/go2rtc directory")
	flag.Parse()
	if *packagesPath == "" || *normalizedPath == "" || *outputRoot == "" {
		fatal(errors.New("-packages, -normalized, and -output are required"))
	}
	if err := collect(*packagesPath, *normalizedPath, *outputRoot); err != nil {
		fatal(err)
	}
}

func collect(packagesPath, normalizedPath, outputRoot string) error {
	input, err := os.Open(packagesPath)
	if err != nil {
		return err
	}
	defer input.Close()

	rawByKey := make(map[string]module)
	decoder := json.NewDecoder(input)
	for {
		var item packageMetadata
		if err = decoder.Decode(&item); errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return fmt.Errorf("decode package stream: %w", err)
		}
		if item.Module == nil {
			continue
		}
		key := moduleKey(*item.Module)
		rawByKey[key] = *item.Module
	}

	rawModules := make([]module, 0, len(rawByKey))
	for _, item := range rawByKey {
		rawModules = append(rawModules, item)
	}
	normalized := make([]normalizedModule, 0, len(rawModules))
	var modules []module
	for _, item := range rawModules {
		normalized = append(normalized, normalizeModule(item))
		if item.Main {
			continue
		}
		if item.Replace != nil {
			item = *item.Replace
		}
		if err = validateModule(item); err != nil {
			return err
		}
		modules = append(modules, item)
	}
	sort.Slice(normalized, func(i, j int) bool {
		if normalized[i].Path == normalized[j].Path {
			return normalized[i].Version < normalized[j].Version
		}
		return normalized[i].Path < normalized[j].Path
	})
	if err = writeModuleStream(normalizedPath, normalized); err != nil {
		return err
	}

	sort.Slice(modules, func(i, j int) bool {
		if modules[i].Path == modules[j].Path {
			return modules[i].Version < modules[j].Version
		}
		return modules[i].Path < modules[j].Path
	})

	modulesRoot := filepath.Join(outputRoot, "modules")
	if err = os.RemoveAll(modulesRoot); err != nil {
		return err
	}
	if err = os.MkdirAll(modulesRoot, 0755); err != nil {
		return err
	}

	manifest := make([]manifestEntry, 0, len(modules))
	seen := make(map[string]struct{}, len(modules))
	for _, item := range modules {
		key := item.Path + "@" + item.Version
		if _, ok := seen[key]; ok {
			return fmt.Errorf("duplicate effective module %s", key)
		}
		seen[key] = struct{}{}

		versionDir := item.Version
		if versionDir == "" {
			versionDir = "local"
		}
		destination := filepath.Join(modulesRoot, filepath.FromSlash(item.Path)+"@"+versionDir)
		files, err := findLicenseFiles(item.Dir)
		if err != nil {
			return fmt.Errorf("module %s: %w", key, err)
		}
		if len(files) == 0 {
			return fmt.Errorf("module %s has no top-level LICENSE/COPYING/NOTICE/COPYRIGHT file", key)
		}
		if err = os.MkdirAll(destination, 0755); err != nil {
			return err
		}

		entry := manifestEntry{Module: item.Path, Version: item.Version}
		for _, source := range files {
			name := filepath.Base(source)
			target := filepath.Join(destination, name)
			digest, err := copyAndHash(source, target)
			if err != nil {
				return err
			}
			relative := filepath.ToSlash(filepath.Join(
				"legal", "go2rtc", "modules", filepath.FromSlash(item.Path)+"@"+versionDir, name,
			))
			entry.LicenseFiles = append(entry.LicenseFiles, licenseFile{Path: relative, SHA256: digest})
		}
		manifest = append(manifest, entry)
	}

	return writeManifest(filepath.Join(outputRoot, "THIRD-PARTY-LICENSES.json"), manifest)
}

func moduleKey(item module) string {
	key := item.Path + "\x00" + item.Version + "\x00" + item.Sum + "\x00" + item.GoModSum
	if item.Replace != nil {
		key += "\x00replace\x00" + moduleKey(*item.Replace)
	}
	return key
}

func normalizeModule(item module) normalizedModule {
	normalized := normalizedModule{
		Path:     item.Path,
		Version:  item.Version,
		Sum:      item.Sum,
		GoModSum: item.GoModSum,
		Main:     item.Main,
	}
	if item.Replace != nil {
		replacement := normalizeModule(*item.Replace)
		normalized.Replace = &replacement
	}
	return normalized
}

func validateModule(item module) error {
	if item.Path == "" || item.Dir == "" {
		return errors.New("module path or source directory is empty")
	}
	if strings.Contains(item.Path, "\\") || strings.HasPrefix(item.Path, "/") {
		return fmt.Errorf("unsafe module path %q", item.Path)
	}
	for _, part := range strings.Split(item.Path, "/") {
		if part == "" || part == "." || part == ".." {
			return fmt.Errorf("unsafe module path %q", item.Path)
		}
	}
	if strings.ContainsAny(item.Version, "/\\") {
		return fmt.Errorf("unsafe module version %q", item.Version)
	}
	return nil
}

func findLicenseFiles(dir string) ([]string, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	var files []string
	for _, entry := range entries {
		if !entry.Type().IsRegular() {
			continue
		}
		upper := strings.ToUpper(entry.Name())
		if strings.HasPrefix(upper, "LICENSE") || strings.HasPrefix(upper, "COPYING") ||
			strings.HasPrefix(upper, "NOTICE") || strings.HasPrefix(upper, "COPYRIGHT") {
			files = append(files, filepath.Join(dir, entry.Name()))
		}
	}
	sort.Strings(files)
	return files, nil
}

func copyAndHash(source, target string) (string, error) {
	input, err := os.Open(source)
	if err != nil {
		return "", err
	}
	defer input.Close()
	output, err := os.OpenFile(target, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0644)
	if err != nil {
		return "", err
	}
	hasher := sha256.New()
	_, copyErr := io.Copy(io.MultiWriter(output, hasher), input)
	closeErr := output.Close()
	if copyErr != nil {
		return "", copyErr
	}
	if closeErr != nil {
		return "", closeErr
	}
	return hex.EncodeToString(hasher.Sum(nil)), nil
}

func writeManifest(path string, manifest []manifestEntry) error {
	file, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0644)
	if err != nil {
		return err
	}
	encoder := json.NewEncoder(file)
	encoder.SetIndent("", "  ")
	err = encoder.Encode(manifest)
	if closeErr := file.Close(); err == nil {
		err = closeErr
	}
	return err
}

func writeModuleStream(path string, modules []normalizedModule) error {
	file, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0644)
	if err != nil {
		return err
	}
	encoder := json.NewEncoder(file)
	encoder.SetIndent("", "  ")
	for _, item := range modules {
		if err = encoder.Encode(item); err != nil {
			break
		}
	}
	if closeErr := file.Close(); err == nil {
		err = closeErr
	}
	return err
}

func fatal(err error) {
	_, _ = fmt.Fprintln(os.Stderr, "collect_licenses:", err)
	os.Exit(1)
}
