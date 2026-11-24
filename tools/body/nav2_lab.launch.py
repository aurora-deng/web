"""Run the installed Nav2 graph in one isolated M08 robot namespace."""
from pathlib import Path

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, GroupAction, IncludeLaunchDescription
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import PushRosNamespace


def generate_launch_description():
    robot = LaunchConfiguration("robot")
    params = LaunchConfiguration("params")
    nav2 = Path(get_package_share_directory("nav2_bringup"))
    return LaunchDescription([
        DeclareLaunchArgument("robot", choices=["alpha", "beta"]),
        DeclareLaunchArgument("params"),
        GroupAction([
            PushRosNamespace(robot),
            IncludeLaunchDescription(
                PythonLaunchDescriptionSource(str(nav2 / "launch" / "navigation_launch.py")),
                launch_arguments={"namespace": robot, "params_file": params, "autostart": "True",
                                  "use_sim_time": "True", "use_composition": "False",
                                  "log_level": "warn"}.items(),
            ),
        ]),
    ])
