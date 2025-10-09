# Installing Client Test Dependencies

## Overview

The Redis Table Module includes optional client compatibility tests for Python and Node.js. This guide shows how to install the required dependencies on different systems.

---

## Python Dependencies

### Ubuntu/Debian (Recommended)

**System Package (Easiest)**:
```bash
sudo apt install python3-redis
```

✅ **Advantages:**
- No virtual environment needed
- Managed by system package manager
- Works with PEP 668 externally-managed environments
- Automatic updates with system

**Version**: Ubuntu 24.04 provides python3-redis 4.3.4

---

### Virtual Environment (Alternative)

If system package is not available or you need a different version:

```bash
# Create virtual environment
python3 -m venv venv

# Activate it
source venv/bin/activate

# Install redis-py
pip install redis

# Run tests
cd tests
python3 test_client_compatibility.py

# Deactivate when done
deactivate
```

✅ **Advantages:**
- Isolated from system Python
- Can install any version
- No sudo required

⚠️ **Note**: Remember to activate the venv before running tests

---

### Why Not Global pip?

**Modern Ubuntu/Debian systems (23.04+) use PEP 668** which prevents global pip installs:

```bash
$ pip install redis
error: externally-managed-environment
```

This is **intentional** to prevent breaking system Python packages. Use system packages or virtual environments instead.

**Don't use `--break-system-packages`** - it can cause system instability.

---

## Node.js Dependencies

### Install Node.js

**Ubuntu/Debian**:
```bash
# Option 1: NodeSource repository (latest version)
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs

# Option 2: Ubuntu repository (older version)
sudo apt install nodejs npm
```

**Verify installation**:
```bash
node --version
npm --version
```

### Install redis Client

```bash
# In the redistable directory
npm install redis
```

Or globally:
```bash
sudo npm install -g redis
```

---

## Verification

### Check Python redis-py

```bash
python3 -c "import redis; print(redis.__version__)"
# Should output: 4.3.4 or similar
```

### Check Node.js redis

```bash
node -e "console.log(require('redis'))"
# Should output: [Object: null prototype] { ... }
```

---

## Running Tests

### After Installing Dependencies

```bash
# Run all client tests
make test-clients

# Or run individually
cd tests
python3 test_client_compatibility.py
node test_client_compatibility.js
```

### Without Dependencies

```bash
# Tests will be skipped gracefully
make test-clients
# Output: ⚠ SKIPPED: redis-py not installed
```

---

## Troubleshooting

### Python: ModuleNotFoundError: No module named 'redis'

**Solution**: Install python3-redis
```bash
sudo apt install python3-redis
```

### Python: externally-managed-environment error

**Solution**: Use system package or virtual environment (see above)

### Node.js: Cannot find module 'redis'

**Solution**: Install redis package
```bash
npm install redis
```

### Node.js: command not found

**Solution**: Install Node.js first
```bash
sudo apt install nodejs npm
```

---

## Platform-Specific Instructions

### Ubuntu 24.04 LTS (Noble)

```bash
# Python
sudo apt install python3-redis

# Node.js (from NodeSource)
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs
npm install redis
```

### Ubuntu 22.04 LTS (Jammy)

```bash
# Python
sudo apt install python3-redis

# Node.js
sudo apt install nodejs npm
npm install redis
```

### Debian 12 (Bookworm)

```bash
# Python
sudo apt install python3-redis

# Node.js
sudo apt install nodejs npm
npm install redis
```

### macOS

```bash
# Python
pip3 install redis

# Node.js (via Homebrew)
brew install node
npm install redis
```

### Other Linux Distributions

**Fedora/RHEL/CentOS**:
```bash
# Python
sudo dnf install python3-redis

# Node.js
sudo dnf install nodejs npm
npm install redis
```

**Arch Linux**:
```bash
# Python
sudo pacman -S python-redis

# Node.js
sudo pacman -S nodejs npm
npm install redis
```

---

## CI/CD Integration

### GitHub Actions

```yaml
- name: Install Python dependencies
  run: |
    sudo apt-get update
    sudo apt-get install -y python3-redis

- name: Install Node.js dependencies
  run: |
    npm install redis

- name: Run client tests
  run: make test-clients
```

### GitLab CI

```yaml
test-clients:
  before_script:
    - apt-get update
    - apt-get install -y python3-redis nodejs npm
    - npm install redis
  script:
    - make test-clients
```

---

## Summary

| Platform | Python Command | Node.js Command |
|----------|---------------|-----------------|
| **Ubuntu/Debian** | `sudo apt install python3-redis` | `sudo apt install nodejs npm && npm install redis` |
| **Fedora/RHEL** | `sudo dnf install python3-redis` | `sudo dnf install nodejs npm && npm install redis` |
| **Arch** | `sudo pacman -S python-redis` | `sudo pacman -S nodejs npm && npm install redis` |
| **macOS** | `pip3 install redis` | `brew install node && npm install redis` |
| **Virtual Env** | `python3 -m venv venv && source venv/bin/activate && pip install redis` | N/A |

---

**Remember**: Client tests are **optional**. The core module tests (`make test`) work without any dependencies!

---

**Version**: 2.1  
**Last Updated**: 2025-10-09  
**Tested On**: Ubuntu 24.04 LTS (Noble Numbat)
