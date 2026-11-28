#!/usr/bin/env python3
"""Run Olive 0.13 with ONNX Runtime GenAI 0.17's loader layout.

Olive 0.13 imports ``onnxruntime_genai.models.quantized_model`` before it
starts ModelBuilder. OGA 0.17 kept the required types, but split that module
between ``models.loaders`` modules. This launcher supplies only the removed
import name in memory and then delegates to Olive's normal CLI. It deliberately
does not edit site-packages or replace either dependency.
"""

from __future__ import annotations

import sys
from types import ModuleType
from typing import Any


def install_oga_017_compatibility_alias() -> None:
    """Expose OGA 0.17 loader classes through Olive 0.13's expected module."""

    from onnxruntime_genai.models.loaders.base import (
        QuantizedDecoderLayer,
        QuantizedModel,
        QuantizedTensorModule,
        TensorModule,
    )
    from onnxruntime_genai.models.loaders.olive import OliveModel
    from onnxruntime_genai.models.loaders.quant_model import QuantModel

    old_name = "onnxruntime_genai.models.quantized_model"
    compatibility_module = ModuleType(old_name)
    compatibility_module.OliveModel = OliveModel
    compatibility_module.QuantModel = QuantModel
    compatibility_module.QuantizedDecoderLayer = QuantizedDecoderLayer
    compatibility_module.QuantizedModel = QuantizedModel
    compatibility_module.QuantizedTensorModule = QuantizedTensorModule
    compatibility_module.TensorModule = TensorModule
    sys.modules[old_name] = compatibility_module

    # OGA 0.17 separated option preparation from create_model(), while Olive
    # 0.13 still calls create_model() directly using the older contract. Add
    # the preparation step at that boundary so the actual builder receives the
    # Hugging Face metadata and validated Adapter path it now requires.
    from onnxruntime_genai.models import builder as oga_builder

    original_create_model = oga_builder.create_model

    def create_model_compat(
        model_name: str,
        input_path: str,
        output_dir: str,
        precision: str,
        execution_provider: str,
        cache_dir: str,
        **extra_options: Any,
    ) -> Any:
        if "hf_details" not in extra_options:
            oga_builder.check_extra_options(
                model_name,
                input_path,
                output_dir,
                precision,
                execution_provider,
                cache_dir,
                extra_options,
            )
        return original_create_model(
            model_name,
            input_path,
            output_dir,
            precision,
            execution_provider,
            cache_dir,
            **extra_options,
        )

    oga_builder.create_model = create_model_compat


def main() -> None:
    install_oga_017_compatibility_alias()

    # Import Olive only after the compatibility alias is ready. Keeping this
    # ordering explicit makes the workaround easy to remove once Olive uses
    # OGA 0.17's current module layout itself.
    from olive.cli.launcher import main as olive_main

    olive_main()


if __name__ == "__main__":
    main()
