from setuptools import setup

setup(
    name="remu",
    version="0.1.0",
    py_modules=["remu_cli"],
    install_requires=["click>=8.0"],
    entry_points={
        "console_scripts": [
            "remu=remu_cli:main",
        ],
    },
)
