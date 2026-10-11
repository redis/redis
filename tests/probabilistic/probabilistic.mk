# Merge retirement only after all five datatype PRs. Missing types fail these
# integration checks rather than silently dropping existing-key coverage.
.PHONY: test-probabilistic-upgrade test-probabilistic-integration
test-probabilistic: test-probabilistic-upgrade test-probabilistic-integration
test-probabilistic-upgrade: $(REDIS_SERVER_NAME)
	$(PYTHON) ../tests/probabilistic/upgrade.py
test-probabilistic-integration: $(REDIS_SERVER_NAME)
	$(PYTHON) ../tests/probabilistic/run.py
